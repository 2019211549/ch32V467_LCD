/*********************************************************************
 * test_t3_swipe.c - T3 用例：横滑切页（iPhone 式分页滚动）
 *
 * 验什么（全部是 LVGL 内置滚动系统，本用例零手势代码）：
 *   1. 跟手：按住拖动，整页 1:1 跟随手指（移动超 SCROLL_LIMIT=10px 后）
 *   2. 回弹：拖过头松手，弹性区把页弹回原位（ELASTIC_FACTOR=4）
 *   3. 吸附：松手/甩动后页自动对中（scroll snap CENTER + 惯性 THROW）
 * 页面 = 3 整屏彩色块（红/蓝/绿）+ 页码，横排在一个 snap 容器里。
 *
 * 两段式（同 test_task_ui：被测行为发生在调度器之后）：
 *   调度器前 test_t3_swipe()：建 t3_ui 任务（被测）+ judge 裁判
 *   调度器后 t3_ui 的自动演示定时器：3s 时程序化滑到第 2 页
 *     （没手指也能看 snap 效果），6s 校验 scroll_x 精确==320；
 *     judge 8s 打判定行。之后界面常驻，随便手滑 —— 手感本来就是
 *     人工验收项（顺滑度、跟手度、回弹观感）。
 *
 * 互斥：与 PSG_TEST_TASK_UI（两个任务各建一套页面抢渲染）、
 * PSG_TEST_BSP_TOUCH（两路轮询抢 I3C）都不能同时开。
 *
 * 手感调参速查：
 *   snap 回位动画时长   本文件 lv_obj_set_style_anim_duration(cont,400)
 *   自动演示动画         本文件 t3_auto_cb 的 lv_anim（500ms 匀速，防撕裂）
 *   弹性/惯性/起滚阈值   lv_conf.h 的 SCROLL_ELASTIC_FACTOR/THROW/LIMIT
 *   帧率（=跟手率）      lv_conf.h 的 LV_DEF_REFR_PERIOD（现 16 = 60fps）
 *********************************************************************/
#include "test_config.h"   /* 开关必须先包含再 #if，写反整文件编译为空 */

#if PSG_TEST_T3_SWIPE

#include "FreeRTOS.h"
#include "task.h"
#include "lvgl.h"
#include "test_framework.h"
#include "bsp_lvgl_port.h"
#include "bsp_lcd.h"
#include "bsp_uart.h"

#define T3_PAGE_W       320
#define T3_PAGE_H       240
#define T3_PAGE_CNT     3

static const uint32_t s_page_color[T3_PAGE_CNT] = {
    0xE23B3B,      /* 页1 红（兼作 BGR/INVON 颜色诊断：红=全对） */
    0x3B82E8,      /* 页2 蓝 */
    0x2FBF71,      /* 页3 绿 */
};

static lv_obj_t *s_cont;               /* snap 容器（自动演示/校验要访问） */
static volatile uint8_t s_touch_ok;    /* t3_ui 记录，judge 读 */
static volatile uint8_t s_scroll_ok;   /* 程序化 snap 是否精确到位 */

/* LVGL 毫秒时基（与 task_ui.c 同款；各文件 static，不建公共文件） */
static uint32_t t3_tick_cb(void)
{
    return (uint32_t)xTaskGetTickCount() * portTICK_PERIOD_MS;
}

/* 自建匀速翻页动画（撕裂优化 B-2，2026-10-14）：
 * LVGL 默认 scroll 动画 = ease_out + 400ms 封顶（lv_obj_scroll.c:356-362 的
 * lv_anim_speed_clamped，不受我们在容器上设的 anim_duration 样式管），起步
 * 最快时一帧内容跳 ~82px —— 帧间错位最宽，正是裂痕最显眼的瞬间。
 * 撕裂错位宽度 = 动画速度 × 帧周期，所以改用 500ms 匀速（峰值=平均速度，
 * 16ms 帧周期下一帧步进恒定 ~10px），裂痕从"一道宽缝"变成运动模糊的一部分 */
static void t3_scroll_exec_cb(void *obj, int32_t v)
{
    lv_obj_scroll_to_x((lv_obj_t *)obj, v, LV_ANIM_OFF);  /* 每步直接落位 */
}

/* 自动演示 + 校验（跑在 t3_ui 的 lv_timer 上下文，LVGL 调用天然单线程）：
 * 第一次到期（3s）：程序化滑到第 2 页（带动画，snap/惯性演示）
 * 第二次到期（6s）：读 scroll_x 校验吸附精度，然后自删 —— 之后全靠手 */
static void t3_auto_cb(lv_timer_t *timer)
{
    static uint8_t step;

    if (step == 0)
    {
        lv_anim_t a;

        lv_anim_init(&a);                          /* 自建动画替代 lv_obj_scroll_to_x */
        lv_anim_set_var(&a, s_cont);               /* （原因见 t3_scroll_exec_cb 注释） */
        lv_anim_set_values(&a, 0, T3_PAGE_W);      /* 页1 → 页2 */
        lv_anim_set_duration(&a, 500);             /* 500ms 匀速 = 一帧步进 ~10px */
        lv_anim_set_path_cb(&a, lv_anim_path_linear);
        lv_anim_set_exec_cb(&a, t3_scroll_exec_cb);
        lv_anim_start(&a);
        step = 1;
    }
    else
    {
        int32_t sx = lv_obj_get_scroll_x(s_cont);

        s_scroll_ok = (sx == T3_PAGE_W) ? 1 : 0;
        uart_printf("t3: auto-snap scroll_x=%d (期望 %d)\r\n", (int)sx, T3_PAGE_W);
        lv_timer_delete(timer);
    }
}

static void t3_ui_task(void *arg)
{
    TickType_t last_stat = xTaskGetTickCount();
    uint8_t touch_rc;
    uint8_t i;

    (void)arg;

    /* 移植层三件套 + tick + 触摸（全幂等，顺序同 task_ui） */
    bsp_lvgl_port_init(LCD_ROT_LANDSCAPE);
    bsp_lvgl_port_async_enable();
    lv_tick_set_cb(t3_tick_cb);
    touch_rc = bsp_lvgl_port_touch_init();
    s_touch_ok = (touch_rc == 0) ? 1 : 0;
    if (touch_rc != 0)
    {
        uart_printf("t3: touch init fail rc=%d（只能看自动演示，手滑无效）\r\n",
                    (int)touch_rc);
    }

    /* 页面：横排 snap 容器 + 3 整屏彩色页。
     * 关键就三行：FLEX_ROW 横排 / SCROLL_DIR_HOR 锁横向 /
     * SNAP_CENTER 松手页对中 —— "iPhone 式分页"全部由这三行触发 */
    s_cont = lv_obj_create(lv_screen_active());
    lv_obj_set_size(s_cont, T3_PAGE_W, T3_PAGE_H);
    lv_obj_center(s_cont);
    lv_obj_set_flex_flow(s_cont, LV_FLEX_FLOW_ROW); //横向拼接 ，容器内部用横向 flex 布局，子对象从左到右排成一行。
    lv_obj_set_style_pad_all(s_cont, 0, 0);            /* 无缝拼接 */
    lv_obj_set_style_border_width(s_cont, 0, 0);
    lv_obj_set_scroll_dir(s_cont, LV_DIR_HOR);
    lv_obj_set_scroll_snap_x(s_cont, LV_SCROLL_SNAP_CENTER); //横向滚动开启吸附：松手后，最近的子对象会被吸附到容器中心。
    lv_obj_set_scrollbar_mode(s_cont, LV_SCROLLBAR_MODE_OFF);
    /* 松手 snap 回位动画时长（撕裂优化 B-3）：250ms 时松手甩页峰值速度
     * ≈3.8px/ms，撕裂错位 ~60px；400ms 减到 ~38px（indev 的 snap/throw 路径
     * 才吃这个样式 —— 自动演示走的是自建动画，不受它管）。观感嫌"粘"就降回 */
    lv_obj_set_style_anim_duration(s_cont, 400, 0);

    for (i = 0; i < T3_PAGE_CNT; i++)
    {
        lv_obj_t *pg  = lv_obj_create(s_cont);
        lv_obj_t *lbl = lv_label_create(pg);

        lv_obj_set_size(pg, T3_PAGE_W, T3_PAGE_H);
        lv_obj_set_style_bg_color(pg, lv_color_hex(s_page_color[i]), 0);
        lv_obj_set_style_border_width(pg, 0, 0);
        lv_obj_set_style_pad_all(pg, 0, 0);
        lv_obj_set_scrollbar_mode(pg, LV_SCROLLBAR_MODE_OFF);
        lv_obj_clear_flag(pg, LV_OBJ_FLAG_SCROLLABLE); /* 页自身不可滚 */
        lv_label_set_text_fmt(lbl, "Page %d/3", (int)(i + 1));
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
        lv_obj_center(lbl);
    }

    lv_timer_create(t3_auto_cb, 3000, NULL);           /* 3s 自动演示，6s 校验 */

    /* 心跳循环：与 task_ui 同款（返回值夹 [1,10]ms 再睡） */
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

        /* 10s 统计（原 5s，60fps 后统计行 ~6ms 的 UART 阻塞会吞帧，降频）：
         * 滑动手感不行时先看这行 —— 滑动期间 flush 增速 = 实际帧率
         * （全屏滚动一帧 = 10 块；60fps 跑满 10s 应 ~6000），err/fb/t_err 恒 0 */
        if ((xTaskGetTickCount() - last_stat) >= pdMS_TO_TICKS(10000))
        {
            uint32_t ds = 0, dd = 0, de = 0, terr = 0;

            bsp_lcd_dma_stats(&ds, &dd, &de);
            bsp_lvgl_port_touch_dbg(0, 0, 0, 0, 0, &terr);
            uart_printf("t3 stat: flush=%lu dma %lu/%lu err=%lu fb=%lu t_err=%lu stk_free=%lu w\r\n",
                        (unsigned long)bsp_lvgl_port_flush_cnt(),
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
 * 汇总行出现属正常（同 test_task_ui） */
static void judge_task(void *arg)
{
    uint32_t start = 0, done = 0, err = 0;
    uint32_t fail0;
    uint32_t flush;

    (void)arg;

    vTaskDelay(pdMS_TO_TICKS(8000));

    bsp_lcd_dma_stats(&start, &done, &err);
    flush = bsp_lvgl_port_flush_cnt();
    fail0 = test_fail_cnt;              /* 判定行只看本段的失败增量 */

    TEST_ASSERT(flush >= 10);           /* 至少刷满一整帧 */
    TEST_ASSERT(s_touch_ok != 0);       /* 触摸在线（手滑路径可用） */
    TEST_ASSERT(s_scroll_ok != 0);      /* 程序化 snap 精确到位 */
    TEST_ASSERT_EQ_INT(start, done);    /* DMA 每笔有始有终 */
    TEST_ASSERT_EQ_INT(err, 0);
    TEST_ASSERT_EQ_INT(bsp_lcd_dma_fallback_cnt(), 0);

    uart_printf("[%s] test_t3_swipe(judge@8s): flush=%lu snap=%d touch=%d dma %lu/%lu err=%lu fb=%lu\r\n",
                (test_fail_cnt == fail0) ? " OK " : "FAIL",
                (unsigned long)flush,
                (int)s_scroll_ok, (int)s_touch_ok,
                (unsigned long)start, (unsigned long)done,
                (unsigned long)err,
                (unsigned long)bsp_lcd_dma_fallback_cnt());

    vTaskDelete(NULL);                  /* 裁判退场，界面留着给人滑 */
}

void test_t3_swipe(void)
{
    /* 栈 1024 字(4KB)：与 task_ui 同级 —— LVGL 渲染链深，2KB 实测溢出过 */
    TEST_ASSERT(xTaskCreate(t3_ui_task, "t3_ui", 1024, NULL, 3, NULL) == pdPASS);
    TEST_ASSERT(xTaskCreate(judge_task, "judge", 256, NULL, 2, NULL) == pdPASS);

    uart_printf("test_t3_swipe: UI+judge 已建，3s 自动演示，8s 判定行，之后随便手滑\r\n");
}

#endif /* PSG_TEST_T3_SWIPE */
