/*********************************************************************
 * test_t4_fullframe_swipe.c - T4 用例：PSRAM 整帧 DIRECT 模式横滑切页
 *
 * 与 T3（test_t3_swipe）【同一套 UI】做 A/B 对比：
 *   T3 = 局部刷新：15K SRAM 小画布 ×2（PARTIAL），一帧分 10 块下发
 *   T4 = 整帧刷新：PSRAM 两块 150K 全帧缓冲（DIRECT 全局渲染），
 *        一帧切 3 条带（320 宽 → 102 行/条，适配 DMA 单笔 32768 半字
 *        上限），逐条异步 DMA —— 全程不退化同步（条带间信号量睡眠，
 *        最后一条在飞时返回，与下一帧渲染流水线重叠）
 *
 * 观察什么：
 *   1. judge@8s 的帧周期 avg/max（T3 的对照数字 = 它 stat 行的 flush
 *      增速；T4 的 flush 数直接就是帧数，DIRECT 一次 flush_cb 一帧）
 *      —— 已知风险点：LVGL 双缓冲 DIRECT 的 refr_sync_areas 会把脏区
 *      CPU 拷到另一块缓冲（全屏 ~150KB，PSRAM 带宽下 ~7ms/帧），
 *      帧周期里能看见这笔账；太重的话 plan B = 单缓冲 DIRECT
 *   2. 手感（人工验收项，和 T3 对比着滑）
 *
 * 两段式同 T3：调度器前建 t4_ui（被测）+ judge；3s 自动演示滑一页，
 * 6s 校验 snap 精度，8s judge 判定行，之后随便手滑。
 *
 * 互斥：与 T3/TASK_UI（各建一套 UI 任务抢渲染）、BSP_TOUCH（抢 I3C）
 * 都不能同时开。A/B 轮流烧：本轮 T4=1/T3=0，看完对调。
 *********************************************************************/
#include "test_config.h"   /* 开关必须先包含再 #if，写反整文件编译为空 */

#if PSG_TEST_T4_FULLFRAME

#include "FreeRTOS.h"
#include "task.h"
#include "lvgl.h"
#include "test_framework.h"
#include "bsp_lvgl_port.h"
#include "bsp_lcd.h"
#include "bsp_uart.h"

#define T4_PAGE_W       320
#define T4_PAGE_H       240
#define T4_PAGE_CNT     3

static const uint32_t s_page_color[T4_PAGE_CNT] = {
    0xE23B3B,      /* 页1 红（兼作 BGR/INVON 颜色诊断：红=全对） */
    0x3B82E8,      /* 页2 蓝 */
    0x2FBF71,      /* 页3 绿 */
};

static lv_obj_t *s_cont;               /* snap 容器（自动演示/校验要访问） */
static volatile uint8_t s_ff_mode;     /* 1 = 整帧模式生效（init 返回值） */
static volatile uint8_t s_touch_ok;    /* t4_ui 记录，judge 读 */
static volatile uint8_t s_scroll_ok;   /* 程序化 snap 是否精确到位 */

/* 帧间隔测量（judge/stat 读，t4_ui 写 → volatile）：
 * DIRECT 模式 flush_cnt 每帧 +1，相邻两帧的 tick 差 = 帧周期（含心跳
 * vTaskDelay）。dt>250ms 的间隔跨了画面静止期（LVGL 不出帧），不算 ——
 * 那是空闲时长，不是性能 */
static volatile uint32_t s_frame_cnt;       /* 观察到的帧完成数 */
static volatile uint32_t s_gap_cnt;         /* 参与统计的帧间隔数 */
static volatile uint32_t s_gap_sum_ticks;   /* 间隔累计（tick） */
static volatile uint32_t s_gap_max_ticks;   /* 最大间隔 */
static uint32_t s_last_fc;                  /* 上次 flush_cnt 快照（单上下文） */
static uint32_t s_last_frame_tick;          /* 上一帧完成时刻 */
static uint8_t  s_have_frame;               /* 已见过至少一帧 */

/* LVGL 毫秒时基（与 task_ui.c 同款；各文件 static，不建公共文件） */
static uint32_t t4_tick_cb(void)
{
    return (uint32_t)xTaskGetTickCount() * portTICK_PERIOD_MS;
}

/* 自建匀速翻页动画（同 T3 撕裂优化 B-2：默认 scroll 动画起步一帧跳
 * ~82px 最显撕裂；500ms 匀速 = 16ms 帧周期下一帧步进恒定 ~10px） */
static void t4_scroll_exec_cb(void *obj, int32_t v)
{
    lv_obj_scroll_to_x((lv_obj_t *)obj, v, LV_ANIM_OFF);  /* 每步直接落位 */
}

/* 自动演示 + 校验（同 T3）：3s 程序化滑到第 2 页，6s 校验 scroll_x */
static void t4_auto_cb(lv_timer_t *timer)
{
    static uint8_t step;

    if (step == 0)
    {
        lv_anim_t a;

        lv_anim_init(&a);
        lv_anim_set_var(&a, s_cont);
        lv_anim_set_values(&a, 0, T4_PAGE_W);      /* 页1 → 页2 */
        lv_anim_set_duration(&a, 500);             /* 500ms 匀速 */
        lv_anim_set_path_cb(&a, lv_anim_path_linear);
        lv_anim_set_exec_cb(&a, t4_scroll_exec_cb);
        lv_anim_start(&a);
        step = 1;
    }
    else
    {
        int32_t sx = lv_obj_get_scroll_x(s_cont);

        s_scroll_ok = (sx == T4_PAGE_W) ? 1 : 0;
        uart_printf("t4: auto-snap scroll_x=%d (期望 %d)\r\n", (int)sx, T4_PAGE_W);
        lv_timer_delete(timer);
    }
}

static void t4_ui_task(void *arg)
{
    TickType_t last_stat = xTaskGetTickCount();
    uint8_t touch_rc;
    uint8_t i;

    (void)arg;

    /* 移植层：整帧 PSRAM 模式（PSRAM 不在线内部自动回退部分刷新并打日志，
     * judge 会判 ff=0 FAIL —— 比黑屏好排查）+ 异步档 + tick + 触摸 */
    s_ff_mode = (bsp_lvgl_port_init_fullframe(LCD_ROT_LANDSCAPE) == 0) ? 1 : 0;
    uart_printf("t4: %s\r\n",
                (s_ff_mode != 0) ? "整帧 DIRECT+PSRAM 双缓冲（对比 T3 局部 SRAM）"
                                 : "已回退部分刷新 —— PSRAM 不在线，A/B 没做成");
    bsp_lvgl_port_async_enable();
    lv_tick_set_cb(t4_tick_cb);
    uart_printf("t4: m1 移植层就绪\r\n");             /* 里程碑：崩在这前=init 路径 */
    touch_rc = bsp_lvgl_port_touch_init();
    s_touch_ok = (touch_rc == 0) ? 1 : 0;
    uart_printf("t4: m2 touch rc=%d\r\n", (int)touch_rc);
    if (touch_rc != 0)
    {
        uart_printf("t4: touch init fail（只能看自动演示，手滑无效）\r\n");
    }

    /* 页面与 T3 逐行相同：横排 snap 容器 + 3 整屏彩色页（变量只有 UI
     * 模式本身，A/B 才是单变量对比） */
    s_cont = lv_obj_create(lv_screen_active());
    lv_obj_set_size(s_cont, T4_PAGE_W, T4_PAGE_H);
    lv_obj_center(s_cont);
    lv_obj_set_flex_flow(s_cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(s_cont, 0, 0);            /* 无缝拼接 */
    lv_obj_set_style_border_width(s_cont, 0, 0);
    lv_obj_set_scroll_dir(s_cont, LV_DIR_HOR);
    lv_obj_set_scroll_snap_x(s_cont, LV_SCROLL_SNAP_CENTER);
    lv_obj_set_scrollbar_mode(s_cont, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_anim_duration(s_cont, 400, 0);    /* snap 回位动画时长 */

    for (i = 0; i < T4_PAGE_CNT; i++)
    {
        lv_obj_t *pg  = lv_obj_create(s_cont);
        lv_obj_t *lbl = lv_label_create(pg);

        lv_obj_set_size(pg, T4_PAGE_W, T4_PAGE_H);
        lv_obj_set_style_bg_color(pg, lv_color_hex(s_page_color[i]), 0);
        lv_obj_set_style_border_width(pg, 0, 0);
        lv_obj_set_style_pad_all(pg, 0, 0);
        lv_obj_set_scrollbar_mode(pg, LV_SCROLLBAR_MODE_OFF);
        lv_obj_clear_flag(pg, LV_OBJ_FLAG_SCROLLABLE); /* 页自身不可滚 */
        lv_label_set_text_fmt(lbl, "Page %d/3", (int)(i + 1));
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
        lv_obj_center(lbl);
    }

    lv_timer_create(t4_auto_cb, 3000, NULL);           /* 3s 自动演示，6s 校验 */
    uart_printf("t4: m3 UI 已建（含首帧渲染未开始）\r\n");

    /* 心跳循环：与 T3 同款（返回值夹 [1,10]ms 再睡，帧周期含这笔） */
    for (;;)
    {
        uint32_t wait = lv_timer_handler();

        if (wait == 0)
        {
            wait = 1;
        }
        if (wait > 10)
        {
            wait = 10;
        }
        vTaskDelay(pdMS_TO_TICKS(wait));

        /* 帧间隔采样：flush 数变了 = 一帧完成（DIRECT 一次 flush_cb 一帧）。
         * 前 3 帧各打一行里程碑 —— 崩在哪一帧前后能从串口直接读出 */
        {
            uint32_t fc  = bsp_lvgl_port_flush_cnt();
            uint32_t now = xTaskGetTickCount();

            if (fc != s_last_fc)
            {
                uint32_t dt = now - s_last_frame_tick;

                if ((s_have_frame != 0) && (dt <= pdMS_TO_TICKS(250)))
                {
                    s_gap_sum_ticks += dt;
                    s_gap_cnt++;
                    if (dt > s_gap_max_ticks)
                    {
                        s_gap_max_ticks = dt;
                    }
                }
                s_frame_cnt++;
                if (s_frame_cnt <= 3u)
                {
                    uart_printf("t4: m4 第%lu帧完成\r\n",
                                (unsigned long)s_frame_cnt);
                }
                s_last_frame_tick = now;
                s_last_fc = fc;
                s_have_frame = 1;
            }
        }

        /* 10s 统计：帧周期 avg/max 是 A/B 的核心数字（对照 T3 的
         * flush 增速换算），strips/frames = 每帧条带数（应为 3） */
        if ((xTaskGetTickCount() - last_stat) >= pdMS_TO_TICKS(10000))
        {
            uint32_t ds = 0, dd = 0, de = 0, terr = 0;
            uint32_t avg_ms = (s_gap_cnt != 0)
                ? (s_gap_sum_ticks * (uint32_t)portTICK_PERIOD_MS) / s_gap_cnt : 0;

            bsp_lcd_dma_stats(&ds, &dd, &de);
            bsp_lvgl_port_touch_dbg(0, 0, 0, 0, 0, &terr);
            uart_printf("t4 stat: ff=%d frames=%lu 周期avg=%lums max=%lums "
                        "strips=%lu dma %lu/%lu err=%lu fb=%lu t_err=%lu stk_free=%lu w\r\n",
                        (int)s_ff_mode,
                        (unsigned long)s_frame_cnt,
                        (unsigned long)avg_ms,
                        (unsigned long)(s_gap_max_ticks * (uint32_t)portTICK_PERIOD_MS),
                        (unsigned long)bsp_lvgl_port_strip_cnt(),
                        (unsigned long)ds, (unsigned long)dd,
                        (unsigned long)de,
                        (unsigned long)bsp_lcd_dma_fallback_cnt(),
                        (unsigned long)terr,
                        (unsigned long)uxTaskGetStackHighWaterMark(NULL));
            last_stat = xTaskGetTickCount();
        }
    }
}

/* 裁判：8s 收数（自动 snap 已完成、校验已跑），判定行晚于 runner
 * 汇总行出现属正常（同 test_task_ui / T3） */
static void judge_task(void *arg)
{
    uint32_t start = 0, done = 0, err = 0;
    uint32_t fail0;
    uint32_t frames, strips, avg_ms;

    (void)arg;

    vTaskDelay(pdMS_TO_TICKS(8000));

    bsp_lcd_dma_stats(&start, &done, &err);
    frames = s_frame_cnt;
    strips = bsp_lvgl_port_strip_cnt();
    avg_ms = (s_gap_cnt != 0)
        ? (s_gap_sum_ticks * (uint32_t)portTICK_PERIOD_MS) / s_gap_cnt : 0;
    fail0 = test_fail_cnt;              /* 判定行只看本段的失败增量 */

    TEST_ASSERT(s_ff_mode != 0);        /* 整帧模式生效（没被回退） */
    TEST_ASSERT(frames >= 10);          /* 3s 起的自动演示至少出过 10 帧 */
    TEST_ASSERT(s_scroll_ok != 0);      /* 程序化 snap 精确到位（==320） */
    TEST_ASSERT(s_touch_ok != 0);       /* 触摸在线（手滑路径可用） */
    TEST_ASSERT(strips >= frames);      /* 每帧至少 1 条带 */
    TEST_ASSERT(strips <= frames * 4u); /* 320 宽全屏一帧 3 条，留余量 */
    TEST_ASSERT_EQ_INT(start, done);    /* DMA 每笔有始有终 */
    TEST_ASSERT_EQ_INT(err, 0);
    TEST_ASSERT_EQ_INT(bsp_lcd_dma_fallback_cnt(), 0);

    uart_printf("[%s] test_t4_fullframe(judge@8s): ff=%d frames=%lu avg=%lums "
                "max=%lums strips=%lu dma %lu/%lu err=%lu fb=%lu\r\n",
                (test_fail_cnt == fail0) ? " OK " : "FAIL",
                (int)s_ff_mode,
                (unsigned long)frames,
                (unsigned long)avg_ms,
                (unsigned long)(s_gap_max_ticks * (uint32_t)portTICK_PERIOD_MS),
                (unsigned long)strips,
                (unsigned long)start, (unsigned long)done,
                (unsigned long)err,
                (unsigned long)bsp_lcd_dma_fallback_cnt());

    vTaskDelete(NULL);                  /* 裁判退场，界面留着给人滑 */
}

void test_t4_fullframe_swipe(void)
{
    /* 栈 2048 字(8KB)：首板 HardFault(mcause=4, 崩在 tick 中断走被踩的
     * 延时链表) 后从 1024 提上来的 —— DIRECT 全帧渲染调用链比 PARTIAL
     * 分块深（全屏 fill/blend + 双缓冲 sync 拷贝），先排除栈溢出踩堆
     * （task_ui 时代 2KB 栈→踩堆→无声 HardFault 的复发同款）。m1~m4
     * 里程碑打印留着：若还崩，串口直接给出崩点阶段 */
    TEST_ASSERT(xTaskCreate(t4_ui_task, "t4_ui", 2048, NULL, 3, NULL) == pdPASS);
    TEST_ASSERT(xTaskCreate(judge_task, "judge", 256, NULL, 2, NULL) == pdPASS);

    uart_printf("test_t4_fullframe_swipe: UI+judge 已建，3s 自动演示，"
                "8s 判定行（帧周期），之后随便手滑\r\n");
}

#endif /* PSG_TEST_T4_FULLFRAME */
