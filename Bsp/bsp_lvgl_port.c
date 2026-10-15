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
 *********************************************************************/
#include "bsp_lvgl_port.h"
#include "bsp_lcd.h"
#include "bsp_touch.h"
#include "lvgl.h"
#include "psg_config.h"     /* PSG_LVGL_DOUBLE_BUF：双画布开关 */
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

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

    data->point.x = s_map_u;
    data->point.y = s_map_v;
    data->state = (s_last_pressed != 0) ? LV_INDEV_STATE_PRESSED
                                        : LV_INDEV_STATE_RELEASED;
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
