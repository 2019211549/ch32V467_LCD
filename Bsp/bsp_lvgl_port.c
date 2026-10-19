/*********************************************************************
 * bsp_lvgl_port.c - LVGL 9.6 ↔ bsp_lcd 胶水层实现
 *
 * LVGL 内存开销（都在 .bss，改大小改这里/改 lv_conf.h）：
 *   - 32K  lv_malloc 内存池（lv_conf.h 的 LV_MEM_SIZE，控件/样式从中分配）
 *   - 15K  部分刷新画布 s_draw_buf（LVGL 先画这块，再整块发给屏）
 *
 * 坐标系：LVGL 与 bsp_lcd 用同一套"目标方向逻辑坐标"——init 先
 * set_rotation 转屏再量宽高注册（横屏 320x240），两边天然一致。
 *
 * 两档刷新方式（运行时切换，不绑死 PSG_MODE，单元测试/App 共用一份代码）：
 *   默认（init 后不调 async_enable）：同步 DMA 轮询 + 立即 ready
 *       —— 检查点 A 语义，调度器没启动也能跑（test_lvgl_hello 用）
 *   bsp_lvgl_port_async_enable() 后：异步 DMA + 完成中断 + 二值信号量
 *       —— 检查点 B 语义，task_ui / test_task_ui 用
 *   bsp_lvgl_port_init_fullframe() 代替 init：整帧 DIRECT 模式 —— 画布
 *       = 两块 PSRAM 全帧缓冲（"全局渲染"），flush_cb 把脏区切条带适配
 *       DMA 单笔上限，逐条仍走异步 DMA（T4 A/B 对比实验；PSRAM 不在线
 *       自动回退部分刷新，不黑屏）
 *********************************************************************/
#include "bsp_lvgl_port.h"
#include "bsp_lcd.h"
#include "bsp_touch.h"
#include "lvgl.h"
#include "psg_config.h"     /* PSG_LVGL_DOUBLE_BUF：双画布开关 + PSG_LOG */
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include <string.h>         /* memset：上电清 PSRAM 帧缓冲 */
#include "bsp_psram.h"      /* 整帧模式的帧缓冲窗口（BSP_PSRAM_WINDOW_BASE） */
#include "bsp_lcd_scan.h"   /* 竖板追扫模式：staging + GSCAN 同步追扫 */
#include "ch32v4x7.h"          /* SPI_BaudRatePrescaler_16（GSCAN 读速档） */
#include "system_ch32v4x7.h"   /* SystemCoreClock：mcycle tick↔µs 换算跟随主频 */

/* 部分刷新画布：按最大逻辑宽 320（横屏）x 24 行 x 2 字节 = 15K。
 * LVGL 按"当前 hor_res"自动折算每块行数：竖屏 240 宽时每块 32 行，
 * 横屏 320 宽时每块 24 行 —— 同一块缓冲两种方向都能用。
 * LV_DRAW_BUF_ALIGN=4（lv_conf.h），静态数组按 4 对齐 */
static uint8_t s_draw_buf[320 * 24 * 2] __attribute__((aligned(4)));

/* 第二块画布（检查点 C，PSG_LVGL_DOUBLE_BUF=1 时启用）：
 * 画块 N 的 DMA 读 buf1 时，LVGL 往 buf2 画块 N+1，交替使用 */
#if PSG_LVGL_DOUBLE_BUF
static uint8_t s_draw_buf2[320 * 24 * 2] __attribute__((aligned(4)));
#endif

/* 教学计数：flush 次数（一次刷新会按画布大小切成 N 块） */
static uint32_t s_flush_cnt;

static lv_display_t *s_disp;        /* init 建的显示屏句柄（async_enable 要用） */
static uint8_t       s_inited;      /* init 幂等保护：重复 lv_init/建屏 = 两块屏挤爆内存池 */
static uint8_t       s_async_flush; /* 1 = 异步档已开启（async_enable 置位） */

static SemaphoreHandle_t s_flush_sem;       /* 二值信号量：DMA 完成 -> 任务 */
static StaticSemaphore_t s_flush_sem_obj;   /* 静态分配，不进 FreeRTOS 堆 */

/* DMA 完成回调（ISR 上下文）：给信号量。二值信号量是"状态量"不是
 * 边沿触发——give 早于 take 到达时计数保持 1，take 立即返回，不丢失 */
static void dma_give_from_isr(void)
{
    BaseType_t woken = pdFALSE;

    xSemaphoreGiveFromISR(s_flush_sem, &woken);  //在 ISR 中释放信号量
    portYIELD_FROM_ISR(woken);              /* 若唤醒了更高优先级任务立刻切 */
}

/* LVGL 复用画布前在此等 DMA 完成；200ms 超时兜底（宁可丢一帧不挂死） */
static void flush_wait_cb(lv_display_t *disp)
{
    (void)disp;
    if (xSemaphoreTake(s_flush_sem, pdMS_TO_TICKS(200)) != pdTRUE)
    {
        bsp_lcd_dma_recover();              /* 超时：强收总线，err 计数 */
    }
}

/* flush_cb：LVGL 画完一块画布后调这里下发。
 * 异步档完整时序（谁在哪个时刻做什么）：
 *   task_ui: lv_timer_handler
 *     -> flush_cb: set_window(CPU) + write_pixels_async(启动 DMA 即返回)
 *     -> LVGL 调 wait_for_flushing (lv_refr.c:1063 复用画布前)
 *        -> flush_wait_cb: xSemaphoreTake 阻塞 —— CPU 让给别的任务
 *   硬件并行: DMA 搬完 -> TC 中断 -> ISR 收总线 + give 信号量
 *   task_ui: take 返回 -> LVGL 清 flushing (lv_refr.c:1496 自己清)
 *            -> 画下一条带
 * 依据 lv_refr.c:1493-1497：设了 wait_cb 时 LVGL 在 wait_cb 返回后自己
 * 清 flushing 标志 —— 正常路径不需要 lv_display_flush_ready */
static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    bsp_lcd_set_window((uint16_t)area->x1, (uint16_t)area->y1,
                       (uint16_t)area->x2, (uint16_t)area->y2);

    if (s_async_flush != 0)
    {
        if (bsp_lcd_write_pixels_async((const uint16_t *)px_map,
            (uint32_t)(area->x2 - area->x1 + 1) * (area->y2 - area->y1 + 1)) == 0)
        {
            s_flush_cnt++; //统计回调次数
            return;        /* 异步正常路径：不调 flush_ready，完成通知全权由 wait_cb 机制给出 */
        }

        /* 降级路径（拒收：奇地址/超大块）——两件必须的事：
         * 1. write_pixels_async 内部已同步发完，这里【绝不能再发一遍】
         *    （旧版 bug：fall through 到下面的同步发送 = 同一块像素发两遍）
         * 2. 排空信号量：同步发完期间 ISR 每笔照常 give 了一个"陈旧令牌"，
         *    不排空的话下一块 wait_cb 会立即吃到旧令牌、提前放行 —— 而
         *    那时新 DMA 还在读画布，CPU 就开始往上画下一块 = 撕裂 */
        while (xSemaphoreTake(s_flush_sem, 0) == pdTRUE) { }

        s_flush_cnt++; //统计回调次数

        lv_display_flush_ready(disp);
        return;
    }

    /* 同步档（检查点 A 语义）：发完才返回，画布立即可以复用 */
    bsp_lcd_write_pixels((const uint16_t *)px_map,
        (uint32_t)(area->x2 - area->x1 + 1) * (area->y2 - area->y1 + 1));

    s_flush_cnt++; //统计回调次数

    lv_display_flush_ready(disp);
}

/* ==================== 整帧 PSRAM 渲染（T4：A/B 对比实验） ====================
 * 与部分刷新的本质差别：LVGL 不再往 15K 小画布上画，画布就是整帧
 * （RENDER_MODE_DIRECT，"全局渲染"），flush_cb 的 px_map = 帧缓冲起点
 * （lv_refr.c 的 call_flush_cb 传 layer->draw_buf->data；其 LV_COLOR_16_SWAP
 * 分支同样按 y1*行距+x1 索引 —— 即脏区首像素 = px_map + y1*行距 + x1）。
 *
 * 一帧 320x240x2 = 153600B，超 DMA 单笔 32768 半字上限（bsp_lcd 的守门
 * 条件）→ 脏区按行切条带：320 宽时 32768/320 = 102 行/条，整屏 240 行
 * = 3 条，逐条 set_window + 异步 DMA：
 *   条带 1..N-1：启动后 xSemaphoreTake 睡眠等本条完成（CPU 让出，不是
 *               轮询也不是同步发）——醒来时 TC 中断已收完总线，所以
 *               下一条 set_window 入口的 dma_sync 防御不会空转
 *   最后一条：  启动后立即返回 —— DMA 在飞期间 LVGL 已开画下一帧
 *               （双缓冲另一块），flush_wait_cb（复用部分刷新的）在
 *               LVGL 复用本帧缓冲前收走最后一个完成通知（lv_refr.c
 *               在换缓冲前 wait_for_flushing）—— 传输与渲染流水线化 */
static uint8_t  *s_ff_fb1;          /* 帧缓冲 A：PSRAM 窗口起点 0x80000000 */
static uint8_t  *s_ff_fb2;          /* 帧缓冲 B：A 后面紧挨着 */
static uint16_t  s_ff_stride_px;    /* 行距（半字）= 逻辑宽，横屏 320 */
static uint8_t   s_ff_active;       /* 1 = 整帧 DIRECT 模式已生效 */
static uint32_t  s_ff_strip_cnt;    /* 累计下发条带数（诊断：每帧几条） */

static void ff_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    const uint16_t *src;
    int32_t w = area->x2 - area->x1 + 1;
    int32_t y = area->y1;
    uint32_t lines_per_strip;
    uint8_t  inflight = 0;          /* 1 = 有一条 DMA 在飞、完成通知未消费 */

    if ((w <= 0) || (y > area->y2))
    {
        lv_display_flush_ready(disp);   /* 空脏区防御 */
        return;
    }

    src = (const uint16_t *)px_map
        + (uint32_t)area->y1 * (uint32_t)s_ff_stride_px + (uint32_t)area->x1;
    lines_per_strip = 32768u / (uint32_t)w;     /* DMA 单笔上限(半字) ÷ 行宽 */

    while (y <= area->y2)
    {
        int32_t h = area->y2 - y + 1;

        if ((uint32_t)h > lines_per_strip)
        {
            h = (int32_t)lines_per_strip;
        }

        bsp_lcd_set_window((uint16_t)area->x1, (uint16_t)y,
                           (uint16_t)area->x2, (uint16_t)(y + h - 1));

        if ((s_async_flush != 0)
            && (bsp_lcd_write_pixels_async(src, (uint32_t)w * (uint32_t)h) == 0))
        {
            inflight = 1;
        }
        else
        {
            /* 同步档，或异步拒收（拒收=内部已同步发完）。同步发完期间
             * ISR 可能 give 过陈旧令牌，排空防下一条提前放行（同部分
             * 刷新档的防御；条带 ≤32768 半字且地址恒偶，理论不会走到） */
            if (s_async_flush != 0)
            {
                while (xSemaphoreTake(s_flush_sem, 0) == pdTRUE) { }
            }
            inflight = 0;
        }

        s_ff_strip_cnt++;

        if ((inflight != 0) && (y + h - 1 < area->y2))
        {
            /* 中间条带：睡眠等本条 DMA 完成（200ms 超时兜底，同 wait_cb） */
            if (xSemaphoreTake(s_flush_sem, pdMS_TO_TICKS(200)) != pdTRUE)
            {
                bsp_lcd_dma_recover();
            }
            inflight = 0;
        }

        src += (uint32_t)h * (uint32_t)s_ff_stride_px;
        y += h;
    }

    s_flush_cnt++;   /* DIRECT 模式一次 flush_cb = 一整帧 */

    if (inflight != 0)
    {
        return;      /* 最后一条在飞：完成通知留给 flush_wait_cb 收 */
    }

    lv_display_flush_ready(disp);   /* 全部同步发出去了：帧已完整上屏 */
}

void bsp_lvgl_port_init(uint8_t rotation)
{
    if (s_inited)
    {
        /* 已初始化过（同一次上电里 hello 用例 + task_ui 都要 init）：
         * 只对齐方向，不重复 lv_init / lv_display_create */
        bsp_lcd_set_rotation(rotation);
        return;
    }
    s_inited = 1;

    bsp_lcd_init();                    /* GPIO/SPI1/ST7789 初始化（默认竖屏） */
    bsp_lcd_set_rotation(rotation);    /* 转到目标方向，只重发 MADCTL 不清屏 */

    lv_init();                         /* LVGL 内部状态初始化（只需一次） */

    /* 按 LCD 当前逻辑分辨率建显示屏（横屏 320x240 / 竖屏 240x320） */  //lvgl可以设置几个
    s_disp = lv_display_create(bsp_lcd_width(), bsp_lcd_height());
    if (s_disp == NULL) {

        return;   /* 内存池不够会走到这：查 lv_conf.h 的 LV_MEM_SIZE。
                     接口不返回错误码，由调用方断言 lv_display_get_default() 兜底 */
    }

    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565); /* 与驱动 COLMOD 0x55 一致 */
    lv_display_set_flush_cb(s_disp, flush_cb);  //当你绘图完成，需要更新 LCD 时，就调用我提供的 flush_cb 函数。”
    /* 告诉 LVGL 用哪块内存绘图/多大/什么刷新模式。第二个参数为 NULL = 单画布；
     * 传第二块 = 双画布（buf_size 是【单块】字节数，两块等大）——LVGL 在
     * 每次调 flush_cb 前先等上一笔 DMA 完成（lv_refr.c:1418），然后换
     * buf_act（:1433-1443），渲染与传输流水线化 */
#if PSG_LVGL_DOUBLE_BUF
    lv_display_set_buffers(s_disp, s_draw_buf, s_draw_buf2, sizeof(s_draw_buf),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);  //双缓冲：画N+1 ∥ 发N
#else
    lv_display_set_buffers(s_disp, s_draw_buf, NULL, sizeof(s_draw_buf),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);  //单缓冲：画下一块前必须等上一块发完
#endif
}

/* 整帧帧缓冲字节数：320x240x2 = 153600B（横竖屏同字节数，都是 76800 像素）。
 * 帧缓冲放 PSRAM 数据窗口（绝对地址指针，不进 Link.ld —— A/B 实验不动
 * 工程身份文件；正式采用后再迁 .psram_bss 段） */
#define FF_FB_BYTES     (320u * 240u * 2u)

uint8_t bsp_lvgl_port_init_fullframe(uint8_t rotation)
{
    if (s_inited)
    {
        /* 幂等：只报当前模式，不重建（LVGL 建屏后缓冲方案不可换 ——
         * 先有人跑过部分刷新的 init 就维持部分刷新） */
        return (s_ff_active != 0) ? 0 : 1;
    }

    /* 前提：PSRAM 在线（init 内含自检，幂等——test_bsp_psram 先跑过也行）。
     * 不在线 → 原部分刷新顶上（调用方拿到 1 能报"对比没做成"，不黑屏） */
    if (bsp_psram_init() != 0)
    {
        PSG_LOG("lvgl_port: PSRAM 不可用，整帧模式回退部分刷新\r\n");
        bsp_lvgl_port_init(rotation);
        return 1;
    }

    s_inited = 1;

    bsp_lcd_init();                    /* 与部分刷新档同一套 LCD/转屏/lv_init */
    bsp_lcd_set_rotation(rotation);
    lv_init();

    /* 上电窗口里是 test_bsp_psram 的图案残留，清两块（CPU 写 ~49MB/s，
     * 2x150KB ≈ 6ms，一次性）。LV_DRAW_BUF_ALIGN=4：0x80000000 与
     * +153600 都满足 */
    s_ff_fb1 = (uint8_t *)BSP_PSRAM_WINDOW_BASE;
    s_ff_fb2 = s_ff_fb1 + FF_FB_BYTES;
    memset(s_ff_fb1, 0, FF_FB_BYTES * 2u);
    s_ff_stride_px = (uint16_t)bsp_lcd_width();

    s_disp = lv_display_create(bsp_lcd_width(), bsp_lcd_height());
    if (s_disp == NULL)
    {
        return 1;   /* 同部分刷新档：内存池不够，调用方断言兜底 */
    }

    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(s_disp, ff_flush_cb);
    /* RENDER_MODE_DIRECT：画布=整帧（buf_size ≥ 一帧），LVGL 直接画在
     * 帧缓冲上，flush_cb 只搬脏区（ff_flush_cb 再切条带适配 DMA 上限）。
     * 双缓冲：LVGL 轮流画 FB_A/FB_B，本帧 DMA(读A) ∥ 下帧渲染(写B)；
     * 代价 = refr_sync_areas 把脏区 CPU 拷到另一块（全屏 ~150KB，
     * PSRAM 带宽下 ~7ms/帧 的已知成本 —— judge 的帧周期实测里可见） */
    lv_display_set_buffers(s_disp, s_ff_fb1, s_ff_fb2, FF_FB_BYTES,
                           LV_DISPLAY_RENDER_MODE_DIRECT);

    s_ff_active = 1;
    PSG_LOG("lvgl_port: 整帧 DIRECT 模式 FB_A=%p FB_B=%p stride=%u\r\n",
            s_ff_fb1, s_ff_fb2, (unsigned)s_ff_stride_px);
    return 0;
}

uint8_t bsp_lvgl_port_fullframe_active(void)
{
    return s_ff_active;
}

uint32_t bsp_lvgl_port_strip_cnt(void)
{
    return s_ff_strip_cnt;   /* 整帧模式诊断：每帧条带数 = strips/frames */
}

/* ==================== 竖板追扫模式（生产路径，2026-10-16 标定收官） ====================
 * 背景：24 行横带写入与面板扫描轴垂直，数学上必有斜向撕裂（v=0.764u，
 * 板证）；本模式改为与扫描同轴的 16 列竖板写入 + GSCAN 回绕对齐。
 * 标定参数（Test/test_lcd_flipcal 板证）：扫描=横屏左→右 52.33µs/列，
 * 回绕早于可见帧头 Δ≈1.1ms；"写一块板(819µs)整段不得与扫描在该板内
 * 重叠" → 起写 δ0=2100µs（零撕裂阈值 1950+余量）、板配速 845µs
 * → 整屏颜色在扫描趟边界统一翻转（帧量化，等效 vsync 页翻转）。
 * 数据通路：LVGL 24 行画布 --band_put--> PSRAM 整帧 staging（staging 兼任
 * 持久帧缓冲：脏区外内容自动保序、无脏区不追扫）→ 帧末 flush_is_last
 * 时 frame_send：等回绕+δ0，20 板 gather(PSRAM→SRAM 乒乓板) ∥ DMA 流水。
 * 时序：整帧 = 渲染 + 回绕等待(0~16.7ms) + 追扫 ~20ms ⇒ 全屏更新周期
 * 2 帧（~30fps 上限）—— 零撕裂的代价；单画布足够（复用画布从不等 LCD） */
static uint8_t s_sc_active;   /* 1 = 竖板追扫模式已生效 */

static void sc_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    (void)bsp_lcd_scan_band_put((uint16_t)area->x1, (uint16_t)area->y1,
                                (uint16_t)area->x2, (uint16_t)area->y2,
                                (const uint16_t *)px_map);
    s_flush_cnt++;

    /* 帧末：staging 已完整 → 追扫上屏（阻塞：回绕等待+20 板流水）。
     * 失败只进诊断计数（stats），不挂 UI —— 下一帧照常重试 */
    if (lv_display_flush_is_last(disp))
    {
        (void)bsp_lcd_scan_frame_send();
    }

    lv_display_flush_ready(disp);   /* 画布立即可复用：LCD 写入全来自 staging */
}

uint8_t bsp_lvgl_port_init_slab(uint8_t rotation)
{
    if (s_inited)
    {
        /* 幂等：报当前模式（同 init_fullframe 语义） */
        return (s_sc_active != 0) ? 0 : 1;
    }

    bsp_lcd_init();                    /* scan_init 的 GSCAN 测量依赖 SPI 就绪 */
    bsp_lcd_set_rotation(rotation);

    if (bsp_lcd_scan_init() != 0)
    {
        PSG_LOG("lvgl_port: scan-init 失败，竖板模式回退部分刷新\r\n");
        bsp_lvgl_port_init(rotation);  /* s_inited 仍 0，走完整部分刷新初始化 */
        return 1;
    }

    s_inited = 1;

    lv_init();

    /* 追扫 DMA 的收尾（CS 释放/DFF 复位/done 计数）靠 TC 中断：注册空
     * 回调占位；之后 async_enable 重注册信号量回调也无害——本模式
     * flush_cb 恒同步 ready，wait_cb 永不会被调到 */
    bsp_lcd_dma_async_enable(0);

    s_disp = lv_display_create(bsp_lcd_width(), bsp_lcd_height());
    if (s_disp == NULL)
    {
        return 1;   /* 同部分刷新档：内存池不够，调用方断言兜底 */
    }

    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(s_disp, sc_flush_cb);
    /* 单画布（双画布无收益：渲染与追扫在本模式是串行的，画布复用也
     * 从不等 LCD —— 帧内流水在 scan 模块 gather∥DMA 里，不在画布层） */
    lv_display_set_buffers(s_disp, s_draw_buf, NULL, sizeof(s_draw_buf),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    s_sc_active = 1;
    PSG_LOG("lvgl_port: 竖板追扫模式生效（staging=PSRAM+0x80000）\r\n");
    return 0;
}

uint8_t bsp_lvgl_port_slab_active(void)
{
    return s_sc_active;
}

/* ==================== 列优先直通模式（T5，2026-10-19 板证定案） ====================
 * 思路：display 按玻璃原生竖屏 240x320 声明（LVGL 坐标 = 竖屏坐标），
 * LCD 侧 MADCTL 换成板证 0x40 —— 屏上画面方向不变，但地址轴含义变为
 * "CASET=屏上竖直方向 v(0..239)、RASET=屏上水平方向 u(0..319，扫描轴)"。
 * 于是 LVGL 部分刷新的横条带（240 宽 x N 行）物理上就是 "N 列 x 全高"
 * 的竖板，画布行主序恰好 = 面板列连续序 → flush_cb 原样 set_window(area)
 * + 一笔异步 DMA：零转置、零拷贝、零 PSRAM（画布即 DMA 源），任意形状
 * 脏区都成立（窗口跟着 area 走）。
 *
 * 撕裂同步（翻色实验第二轮板证参数：T_scan=16747µs、扫描 ~50.1µs/可见列、
 * 16 列板 DMA 819µs、Δ≈1165µs、背靠背 20 板 ≈16.6ms < 1 帧 → δ0=0 同帧
 * 干净翻色）：每刷新周期【第一条】flush 前等 GSCAN 回绕（δ0=0），随后
 * 条带背靠背连发，写前沿沿扫描轴领先并同帧内跑完 = 等效 vsync 页翻转，
 * 且无 T3 追扫的"整帧量化"等待（2 帧周期 ~30fps）→ 理论 1 帧周期 ~60fps。
 * 等回绕时总线必然空闲：双画布下 LVGL 调 flush_cb 前已 wait_for_flushing
 * 排干上一周期末条 DMA（lv_refr.c:1418），GSCAN 读不与流式 DMA 撞车。
 * SDO 不在线则放弃同步照发（可能撕裂，sync_miss 计数标出）。
 *
 * 条带高 16 列：与翻色标定实验同宽（板证数字 1:1 迁移），全屏 320/16=20
 * 条；画布 2x240x16x2=15K SRAM（与部分刷新档同预算） */
#define CMP_STRIP_H        16u                          /* 条带=16 列竖板 */
#define CMP_GSCAN_CMD      0x45
#define CMP_RD_PRESC       SPI_BaudRatePrescaler_16     /* 板证最快可信读档 ~5µs/次 */
#define CMP_WRAP_DROP      (-128)                       /* 回绕判据：计数大幅回落 */
#define CMP_WRAP_TIMEOUT_MS 60u                         /* 回绕等待兜底（1 帧 16.7ms） */

static uint8_t  s_cm_buf1[LCD_NATIVE_W * CMP_STRIP_H * 2u] __attribute__((aligned(4)));
static uint8_t  s_cm_buf2[LCD_NATIVE_W * CMP_STRIP_H * 2u] __attribute__((aligned(4)));
static uint8_t  s_cm_active;       /* 1 = 列优先直通模式已生效 */
static uint8_t  s_cm_cycle_first = 1;   /* 本刷新周期还没做过回绕对齐 */
static uint32_t s_cm_frames;       /* 完成的刷新周期数（相邻回绕间隔计一帧） */
static uint32_t s_cm_sync_miss;    /* 回绕等待超时次数（SDO 不在线/读异常） */
static uint64_t s_cm_period_sum;   /* 周期累计(µs)，除以帧数=avg */
static uint32_t s_cm_period_max;   /* 周期最大值(µs) */
static uint32_t s_cm_last_wrap_tick;   /* 上次回绕检出时刻(mcycle)，0=尚无 */

static inline uint32_t cmp_cyc(void)
{
    uint32_t v;
    __asm volatile ("csrr %0, mcycle" : "=r" (v));
    return v;
}

static uint32_t cmp_ticks_to_us(uint32_t t)
{
    return (uint32_t)(((uint64_t)t * 1000000ull) / SystemCoreClock);
}

/* GSCAN 读当前扫描行（colmajor/flipcal 同款）：事务中点作时间戳 */
static uint16_t cmp_gscan(uint32_t *t_mid)
{
    uint8_t  b[2];
    uint32_t t0 = cmp_cyc();

    (void)bsp_lcd_read_reg_p(CMP_GSCAN_CMD, 1, b, 2, CMP_RD_PRESC);
    *t_mid = t0 + ((cmp_cyc() - t0) >> 1);
    return (uint16_t)(((uint16_t)b[0] << 8) | b[1]);
}

/* 等回绕：返回检出时刻(tick)，超时返回 0。检出精度 ~5µs */
static uint32_t cmp_wait_wrap(uint32_t timeout_ms)
{
    uint32_t t0 = cmp_cyc();
    uint32_t lim = timeout_ms * (SystemCoreClock / 1000u);
    uint32_t t;
    uint16_t prev = cmp_gscan(&t);

    for (;;)
    {
        uint16_t v = cmp_gscan(&t);

        if ((int32_t)v - (int32_t)prev <= CMP_WRAP_DROP) { return t; }
        prev = v;
        if ((cmp_cyc() - t0) > lim) { return 0; }
    }
}

/* flush_cb：area 原样即窗口（LVGL-x=CASET=v 全跨或部分，LVGL-y=RASET=u）。
 * 周期首条先做回绕对齐（阻塞 0~16.7ms，与 T3 frame_send 同量级） */
static void cm_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    if (s_cm_cycle_first != 0)
    {
        uint32_t tw = cmp_wait_wrap(CMP_WRAP_TIMEOUT_MS);

        if (tw != 0u)
        {
            if (s_cm_last_wrap_tick != 0u)     /* 相邻回绕间隔=刷新周期 */
            {
                uint32_t us = cmp_ticks_to_us(tw - s_cm_last_wrap_tick);

                s_cm_period_sum += us;
                if (us > s_cm_period_max) { s_cm_period_max = us; }
                s_cm_frames++;
            }
            s_cm_last_wrap_tick = tw;
        }
        else
        {
            s_cm_sync_miss++;   /* 无回绕（SDO 不在线？）：放弃对齐照发 */
        }
        s_cm_cycle_first = 0;
    }

    bsp_lcd_set_window((uint16_t)area->x1, (uint16_t)area->y1,
                       (uint16_t)area->x2, (uint16_t)area->y2);

    if (bsp_lcd_write_pixels_async((const uint16_t *)px_map,
        (uint32_t)(area->x2 - area->x1 + 1) * (area->y2 - area->y1 + 1)) == 0)
    {
        s_flush_cnt++;
        if (lv_display_flush_is_last(disp)) { s_cm_cycle_first = 1; }
        return;   /* 异步正常路径：完成通知由 wait_cb 机制给出（同 async 档） */
    }

    /* 拒收兜底（理论到不了：条带 3840 半字、地址恒偶）：同步已发完，
     * 排空陈旧令牌防下一条提前放行（同部分刷新档的防御） */
    while (xSemaphoreTake(s_flush_sem, 0) == pdTRUE) { }

    s_flush_cnt++;
    if (lv_display_flush_is_last(disp)) { s_cm_cycle_first = 1; }
    lv_display_flush_ready(disp);
}

uint8_t bsp_lvgl_port_init_colmajor(void)
{
    if (s_inited)
    {
        /* 幂等：报当前模式（同 init_fullframe/init_slab 语义） */
        return (s_cm_active != 0) ? 0 : 1;
    }
    s_inited = 1;

    bsp_lcd_init();                    /* GPIO/SPI1/ST7789 初始化（默认竖屏） */
    bsp_lcd_set_rotation(LCD_ROT_LANDSCAPE);   /* 标准初始化序列（MADCTL 0x60）走一遍 */
    bsp_lcd_set_madctl_raw(0x40);      /* 换列优先地址序；屏上方向不变（板证） */

    lv_init();

    /* 异步三件套在本模式 init 内一步到位（flush 恒异步，无同步档需求；
     * 之后调 async_enable 会因 s_flush_sem 非空幂等跳过，无害） */
    s_flush_sem = xSemaphoreCreateBinaryStatic(&s_flush_sem_obj);
    bsp_lcd_dma_async_enable(dma_give_from_isr);

    /* 按玻璃原生竖屏建显示屏：LVGL-x=CASET=v（屏上竖直），LVGL-y=RASET=u
     * （屏上水平/扫描轴）。宽高用固定玻璃尺寸（不查 bsp_lcd_width ——
     * 内部 rotation 状态还是 LANDSCAPE，会报 320x240） */
    s_disp = lv_display_create(LCD_NATIVE_W, LCD_NATIVE_H);
    if (s_disp == NULL)
    {
        return 1;   /* 同部分刷新档：内存池不够，调用方断言兜底 */
    }

    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(s_disp, cm_flush_cb);
    lv_display_set_flush_wait_cb(s_disp, flush_wait_cb);
    /* 双画布（T5 的流水关键：画条带 N+1 ∥ 发条带 N；LVGL 复用画布前经
     * wait_cb 排干在飞 DMA —— 也是周期首条 GSCAN 读的安全前提） */
    lv_display_set_buffers(s_disp, s_cm_buf1, s_cm_buf2, sizeof(s_cm_buf1),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    s_async_flush = 1;    /* 共用标志：表明异步档已开（语义一致） */
    s_cm_active = 1;
    PSG_LOG("lvgl_port: 列优先直通模式生效（240x320 声明 MADCTL=0x40 "
            "条带=%u列 GSCAN 回绕对齐）\r\n", (unsigned)CMP_STRIP_H);
    return 0;
}

uint8_t bsp_lvgl_port_cm_active(void)
{
    return s_cm_active;
}

void bsp_lvgl_port_cm_stats(uint32_t *frames, uint32_t *avg_period_us,
                            uint32_t *max_period_us, uint32_t *sync_miss)
{
    if (frames        != 0) { *frames        = s_cm_frames; }
    if (avg_period_us != 0)
    {
        *avg_period_us = (s_cm_frames != 0u)
                       ? (uint32_t)(s_cm_period_sum / s_cm_frames) : 0u;
    }
    if (max_period_us != 0) { *max_period_us = s_cm_period_max; }
    if (sync_miss     != 0) { *sync_miss     = s_cm_sync_miss; }
}

void bsp_lvgl_port_async_enable(void)
{
    if (s_disp == NULL || s_flush_sem != NULL) {
        return;   /* init 没跑成 / 异步档已开启：幂等 */
    }

    /* 检查点 B 三件套：静态二值信号量 + DMA 完成中断 + LVGL 等待钩子。
     * 之后 flush_cb 走 write_pixels_async（启动 DMA 即返回），LVGL 复用
     * 画布前阻塞在信号量上等 DMA 完成中断（flush_wait_cb） */
    s_flush_sem = xSemaphoreCreateBinaryStatic(&s_flush_sem_obj); //创建了一个二值信号量 //这个函数把 s_flush_sem_obj 这块裸内存，变成了一个能用的信号量，并把它的地址赋给了 s_flush_sem。

    bsp_lcd_dma_async_enable(dma_give_from_isr); //打开 LCD 的 DMA 异步通道
    lv_display_set_flush_wait_cb(s_disp, flush_wait_cb);
    s_async_flush = 1;
}

uint32_t bsp_lvgl_port_flush_cnt(void)
{
    return s_flush_cnt;
}

/* ==================== 触摸 indev（T2：LVGL 轮询式指针设备） ====================
 * 模型：不建触摸任务 —— lv_indev_create 自带一个 33ms 的读定时器
 * （LV_DEF_REFR_PERIOD，与刷新周期同一个宏，lv_indev.c:105），lv_timer_handler
 * 里周期调 touch_read_cb，
 * 和渲染同在 task_ui 上下文，无锁。read_cb 只管填"当前状态+当前点"，
 * 按下/抬起/点击事件全部由 LVGL 状态机自己合成。
 *
 * 坐标映射（板证 2026-10-13，映射②）：玻璃原生竖屏 240x320，屏以横屏
 * 装配 —— 摸屏幕物理左上角玻璃读数≈(239,0)，右下≈(0,319)，故
 *   逻辑x(u,0..319) = 玻璃y(gy)      逻辑y(v,0..239) = 239 - 玻璃x(gx)
 * 四角实测：TL 玻璃(239,25)→(25,0)  BR 玻璃(0,302)→(302,239)（指腹偏移内）
 * 只配了横屏（当前产品形态）；换方向要连映射一起改，别只转屏 */
static uint8_t  s_touch_inited;    /* 幂等保护，与 s_inited 同款 */
static uint8_t  s_touch_portrait; /* 1 = T5 列优先竖屏声明：indev 坐标换轴 */
static uint16_t s_last_gx, s_last_gy;      /* read_cb 最近一次成功读数（日志用） */
static uint16_t s_map_u, s_map_v;          /* 对应的映射后逻辑坐标 */
static uint8_t  s_last_pressed;            /* 最近一次按下状态 */
static uint32_t s_touch_err;               /* read_cb 读失败计数（长期健康指标） */

void bsp_lvgl_port_touch_map(uint16_t gx, uint16_t gy, uint16_t *u, uint16_t *v)
{
    int32_t uu = (int32_t)gy;               /* 映射②，见上 */
    int32_t vv = 239 - (int32_t)gx;

    if (uu < 0)     { uu = 0; }
    else if (uu > 319) { uu = 319; }        /* 边缘越界钳位（指腹压边会读出界） */
    if (vv < 0)     { vv = 0; }
    else if (vv > 239) { vv = 239; }
    *u = (uint16_t)uu;
    *v = (uint16_t)vv;
}

/* read_cb 契约：每次填 data->state + data->point 即可。
 * 读失败时沿用上次状态 —— 总线抖一下不能变成假抬起/假按下 */
static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    uint16_t gx = 0;
    uint16_t gy = 0;
    uint8_t  pressed = 0;

    (void)indev;

    if (bsp_touch_read(&gx, &gy, &pressed) == 0)
    {
        uint16_t u = 0;
        uint16_t v = 0;

        bsp_lvgl_port_touch_map(gx, gy, &u, &v);  //坐标映射
        s_last_gx = gx;
        s_last_gy = gy;
        s_map_u   = u;
        s_map_v   = v;
        s_last_pressed = pressed;
    }
    else
    {
        s_touch_err++;
    }

    if (s_touch_portrait != 0)
    {
        /* T5 列优先模式：display 按竖屏声明 → LVGL-x=屏v=239-gx、
         * LVGL-y=屏u=gy。映射②算出的 (u,v) 直接换轴即可，方向正确性
         * 继承横屏板证（在 touch_set_portrait(1) 后、任意时刻可切） */
        data->point.x = s_map_v;
        data->point.y = s_map_u;
    }
    else
    {
        data->point.x = s_map_u;
        data->point.y = s_map_v;
    }
    data->state = (s_last_pressed != 0) ? LV_INDEV_STATE_PRESSED
                                        : LV_INDEV_STATE_RELEASED;
}

void bsp_lvgl_port_touch_set_portrait(uint8_t en)
{
    s_touch_portrait = (en != 0) ? 1 : 0;
}

uint8_t bsp_lvgl_port_touch_init(void)
{
    lv_indev_t *indev;

    if (s_touch_inited != 0)  { return 0; }   /* 幂等 */  //防止重复创建
    if (s_disp == NULL)       { return 2; }   /* 先 bsp_lvgl_port_init */
    if (bsp_touch_init() != 0){ return 1; }   /* 触摸不在线：返回错误码但不挡显示 */

    indev = lv_indev_create();                /* 自带 33ms 读定时器（LVGL 实现在 lv_indev.c:88） */
    if (indev == NULL)        { return 3; }   /* 内存池不够（LV_MEM_SIZE） */
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read_cb);
    /* 不调 lv_indev_set_display：绑定默认屏（当前唯一屏） */

    s_touch_inited = 1;
    return 0;
}

void bsp_lvgl_port_touch_dbg(uint16_t *gx, uint16_t *gy, uint16_t *u,
                             uint16_t *v, uint8_t *pressed, uint32_t *err)
{
    if (gx      != 0) { *gx      = s_last_gx;      }
    if (gy      != 0) { *gy      = s_last_gy;      }
    if (u       != 0) { *u       = s_map_u;        }
    if (v       != 0) { *v       = s_map_v;        }
    if (pressed != 0) { *pressed = s_last_pressed; }
    if (err     != 0) { *err     = s_touch_err;    }
}
