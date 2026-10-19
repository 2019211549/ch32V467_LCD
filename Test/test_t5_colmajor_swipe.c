/*********************************************************************
 * test_t5_colmajor_swipe.c - T5 用例：列优先直通模式滑动切页
 *
 * 底座 = 2026-10-19 板证收官的列优先写 GRAM（LCD 深入学习 2/2 课）：
 *   display 按玻璃原生竖屏 240x320 声明 + MADCTL=0x40 → LVGL 部分
 *   刷新的 240x16 横条带物理上就是 16 列 x 全高竖板，画布行主序恰好
 *   = 面板列连续序 → flush_cb 原样 set_window + 一笔异步 DMA
 *   （零转置/零拷贝/零 PSRAM，画布即 DMA 源）。
 * 撕裂同步 = 翻色实验第二轮回证：每周期首条 flush 前 GSCAN 回绕对齐
 *   （δ0=0）+ 20 板背靠背 ≈16.6ms < 1 帧 → 写前沿同帧内领先跑完
 *   = 等效 vsync 页翻转；且无 T3 追扫的"整帧量化"（2 帧周期 ~30fps），
 *   本模式理论 1 帧周期 ~60fps —— 与 T3 A/B 对比手感就是本课结论。
 *
 * UI（用户指定形态：一个 screen + 多容器；与 T3 同款翻页，竖屏坐标搭）：
 *   screen 240x320 → 竖排 flex 容器 → 3 页 240x320 叠放 y=0/320/640，
 *   scroll_dir=VER + snap_y(CENTER) —— 屏以横屏装配，竖屏坐标的 y 轴
 *   投影到屏上是物理水平方向：LVGL 里的"上下滑"在屏上看就是"左右滑"。
 *   文字横躺 90°（坐标系整体转 90° 的已知代价，机制验证课不纠）。
 *   触摸同轴换算（映射②换轴）：LVGL-x=239-玻璃x、LVGL-y=玻璃y。
 *
 * 两段式（同 T3）：调度器前建 t5_ui(被测)+judge(裁判)；3s 自动演示
 *   匀速滑一页（500ms 自建动画），6s 校验 scroll_y==320，8s judge 判定行
 *   （含 T5 专属：cm 帧数≥5、sync_miss==0、帧周期 avg/max），
 *   之后界面常驻随便手滑 —— 手感与 T3 对比是人工验收项。
 *
 * 观察点：
 *   1. 手指沿屏长边（装配横向）滑动 → 翻页 + 吸附回弹（snap 400ms）
 *   2. 滑动全程无竖向/横向撕裂线（回绕对齐 + 同帧翻色的兑现）
 *   3. 串口 10s 心跳：flush/dma/cm 帧周期 avg-max/同步失败数/栈水位
 *
 * 互斥：与 T3/T4（同款 UI 任务抢渲染）、PSG_TEST_BSP_TOUCH（抢 I3C）
 * 都不能同时开 —— A/B 对比时轮流置 1。
 *********************************************************************/
#include "test_config.h"   /* 开关必须先包含再 #if，写反整文件编译为空 */

#if PSG_TEST_T5_COLMAJOR_SWIPE

#include "FreeRTOS.h"
#include "task.h"
#include "lvgl.h"
#include "test_framework.h"
#include "bsp_lvgl_port.h"
#include "bsp_lcd.h"       /* LCD_ROT_LANDSCAPE */
#include "bsp_uart.h"

#define T5_PAGE_W       240    /* 竖屏页宽 = 玻璃短边 */
#define T5_PAGE_H       320    /* 竖屏页高 = 玻璃长边（滑一页 = 320px） */
#define T5_PAGE_CNT     3

static const uint32_t s_page_color[T5_PAGE_CNT] = {
    0xE23B3B,      /* 页1 红（兼作 BGR/INVON 颜色诊断：红=全对） */
    0x3B82E8,      /* 页2 蓝 */
    0x2FBF71,      /* 页3 绿 */
};

static lv_obj_t *s_cont;               /* snap 容器（自动演示/校验要访问） */
static volatile uint8_t s_touch_ok;    /* t5_ui 记录，judge 读 */
static volatile uint8_t s_scroll_ok;   /* 程序化 snap 是否精确到位 */

/* LVGL 毫秒时基（与 task_ui.c 同款；各文件 static，不建公共文件） */
static uint32_t t5_tick_cb(void)
{
    return (uint32_t)xTaskGetTickCount() * portTICK_PERIOD_MS;
}

/* 自建匀速翻页动画（同 T3 的撕裂优化 B-2 理由）：LVGL 默认 scroll 动画
 * 是 ease_out，起步一帧内容跳 ~80px —— 错位最宽的瞬间最显裂痕；改 500ms
 * 匀速（峰值=平均速度，16ms 帧周期下一帧步进恒定 ~13px），滑进运动模糊 */
static void t5_scroll_exec_cb(void *obj, int32_t v)
{
    lv_obj_scroll_to_y((lv_obj_t *)obj, v, LV_ANIM_OFF);  /* 每步直接落位 */
}

/* 自动演示 + 校验（跑在 t5_ui 的 lv_timer 上下文）：
 * 第一次到期（3s）：程序化滑到第 2 页（带动画）
 * 第二次到期（6s）：读 scroll_y 校验吸附精度，然后自删 —— 之后全靠手 */
static void t5_auto_cb(lv_timer_t *timer)
{
    static uint8_t step;

    if (step == 0)
    {
        lv_anim_t a;

        lv_anim_init(&a);
        lv_anim_set_var(&a, s_cont);
        lv_anim_set_values(&a, 0, T5_PAGE_H);      /* 页1 → 页2 */
        lv_anim_set_duration(&a, 500);             /* 500ms 匀速 = 一帧步进 ~13px */
        lv_anim_set_path_cb(&a, lv_anim_path_linear);
        lv_anim_set_exec_cb(&a, t5_scroll_exec_cb);
        lv_anim_start(&a);
        step = 1;
    }
    else
    {
        int32_t sy = lv_obj_get_scroll_y(s_cont);

        s_scroll_ok = (sy == T5_PAGE_H) ? 1 : 0;
        uart_printf("t5: auto-snap scroll_y=%d (期望 %d)\r\n", (int)sy, T5_PAGE_H);
        lv_timer_delete(timer);
    }
}

static void t5_ui_task(void *arg)
{
    TickType_t last_stat = xTaskGetTickCount();
    uint8_t cm_rc;
    uint8_t touch_rc;
    uint8_t i;

    (void)arg;

    /* 列优先直通底座（无参数，方向固定）：240x320 声明 + MADCTL=0x40 +
     * GSCAN 回绕对齐 + 双画布条带 DMA，异步三件套 init 内已就位 */
    cm_rc = bsp_lvgl_port_init_colmajor();
    if (cm_rc != 0)
    {
        uart_printf("t5: 列优先 init rc=%d（本次上电已被其它模式初始化？）\r\n",
                    (int)cm_rc);
    }
    lv_tick_set_cb(t5_tick_cb);

    /* 触摸：先切竖屏坐标轴再 init（read_cb 每次读都看标志，先后无副作用） */
    bsp_lvgl_port_touch_set_portrait(1);
    touch_rc = bsp_lvgl_port_touch_init();
    s_touch_ok = (touch_rc == 0) ? 1 : 0;
    if (touch_rc != 0)
    {
        uart_printf("t5: touch init fail rc=%d（只能看自动演示，手滑无效）\r\n",
                    (int)touch_rc);
    }

    /* 页面：一个 screen + 竖排 snap 容器 + 3 整屏彩色页（用户指定形态）。
     * 关键三行：FLEX_COLUMN 竖排 / SCROLL_DIR_VER 锁竖向 /
     * SNAP_CENTER 松手页对中 —— 屏横着装，竖屏 y 轴在屏上是水平方向，
     * 观感即"左右滑分页" */
    s_cont = lv_obj_create(lv_screen_active());
    lv_obj_set_size(s_cont, T5_PAGE_W, T5_PAGE_H);
    lv_obj_center(s_cont);
    lv_obj_set_flex_flow(s_cont, LV_FLEX_FLOW_COLUMN);  /* 竖向拼接（竖屏坐标） */
    lv_obj_set_style_pad_all(s_cont, 0, 0);             /* 无缝拼接 */
    lv_obj_set_style_border_width(s_cont, 0, 0);
    lv_obj_set_scroll_dir(s_cont, LV_DIR_VER);
    lv_obj_set_scroll_snap_y(s_cont, LV_SCROLL_SNAP_CENTER); /* 松手页对中 */
    lv_obj_set_scrollbar_mode(s_cont, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_anim_duration(s_cont, 400, 0);     /* snap 回位动画（同 T3 调参） */

    for (i = 0; i < T5_PAGE_CNT; i++)
    {
        lv_obj_t *pg  = lv_obj_create(s_cont);
        lv_obj_t *lbl = lv_label_create(pg);

        lv_obj_set_size(pg, T5_PAGE_W, T5_PAGE_H);
        lv_obj_set_style_bg_color(pg, lv_color_hex(s_page_color[i]), 0);
        lv_obj_set_style_border_width(pg, 0, 0);
        lv_obj_set_style_pad_all(pg, 0, 0);
        lv_obj_set_scrollbar_mode(pg, LV_SCROLLBAR_MODE_OFF);
        lv_obj_clear_flag(pg, LV_OBJ_FLAG_SCROLLABLE); /* 页自身不可滚 */
        lv_label_set_text_fmt(lbl, "Page %d/3", (int)(i + 1));  /* 屏上横躺 90°，预期内 */
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
        lv_obj_center(lbl);
    }

    lv_timer_create(t5_auto_cb, 3000, NULL);           /* 3s 自动演示，6s 校验 */

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

        /* 10s 统计（同 T3）：滑动期间 flush 增速=实际帧率（全屏滚动一帧
         * = 20 条带；60fps 跑满 10s 应 ~20000），err/fb/t_err 恒 0 */
        if ((xTaskGetTickCount() - last_stat) >= pdMS_TO_TICKS(10000))
        {
            uint32_t ds = 0, dd = 0, de = 0, terr = 0;

            bsp_lcd_dma_stats(&ds, &dd, &de);
            bsp_lvgl_port_touch_dbg(0, 0, 0, 0, 0, &terr);
            uart_printf("t5 stat: flush=%lu dma %lu/%lu err=%lu fb=%lu t_err=%lu stk_free=%lu w\r\n",
                        (unsigned long)bsp_lvgl_port_flush_cnt(),
                        (unsigned long)ds, (unsigned long)dd,
                        (unsigned long)de,
                        (unsigned long)bsp_lcd_dma_fallback_cnt(),
                        (unsigned long)terr,
                        (unsigned long)uxTaskGetStackHighWaterMark(NULL));
            if (bsp_lvgl_port_cm_active() != 0)       /* 列优先专属行 */
            {
                uint32_t fr = 0, avg = 0, mx = 0, miss = 0;

                bsp_lvgl_port_cm_stats(&fr, &avg, &mx, &miss);
                uart_printf("t5 stat: 列优先 帧%lu 周期avg %lums max %lums "
                            "sync_miss=%lu\r\n",
                            (unsigned long)fr, (unsigned long)(avg / 1000u),
                            (unsigned long)(mx / 1000u), (unsigned long)miss);
            }
            last_stat = xTaskGetTickCount();
        }
    }
}

/* 裁判：8s 收数（自动 snap 已完成、校验已跑），判定行晚于 runner
 * 汇总行出现属正常（同 T3/test_task_ui） */
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

    TEST_ASSERT(flush >= 10);           /* 至少刷满一整帧（20 条带） */
    TEST_ASSERT(s_touch_ok != 0);       /* 触摸在线（手滑路径可用） */
    TEST_ASSERT(s_scroll_ok != 0);      /* 程序化 snap 精确到位 */
    TEST_ASSERT_EQ_INT(start, done);    /* DMA 每笔有始有终 */
    TEST_ASSERT_EQ_INT(err, 0);
    TEST_ASSERT_EQ_INT(bsp_lcd_dma_fallback_cnt(), 0);

    /* 列优先专属判定（模式没生效时 cm_active=0，整段跳过）：
     * 8s 内初始渲染 + 500ms 动画 + indev 轮询至少攒 5 个回绕对齐周期；
     * sync_miss 必须 0 —— 非零说明 SDO 不在线/读异常，"无撕裂"结论不成立 */
    if (bsp_lvgl_port_cm_active() != 0)
    {
        uint32_t fr = 0, avg = 0, mx = 0, miss = 0;

        bsp_lvgl_port_cm_stats(&fr, &avg, &mx, &miss);
        TEST_ASSERT(fr >= 5);
        TEST_ASSERT_EQ_INT(0, (int)miss);

        uart_printf("t5 cm(judge@8s): 帧%lu 周期avg %lums max %lums sync_miss=%lu "
                    "—— 滑动无撕裂线 + 手感对比 T3 即收官\r\n",
                    (unsigned long)fr, (unsigned long)(avg / 1000u),
                    (unsigned long)(mx / 1000u), (unsigned long)miss);
    }

    uart_printf("[%s] test_t5_swipe(judge@8s): flush=%lu snap=%d touch=%d dma %lu/%lu err=%lu fb=%lu cm=%d\r\n",
                (test_fail_cnt == fail0) ? " OK " : "FAIL",
                (unsigned long)flush,
                (int)s_scroll_ok, (int)s_touch_ok,
                (unsigned long)start, (unsigned long)done,
                (unsigned long)err,
                (unsigned long)bsp_lcd_dma_fallback_cnt(),
                (int)bsp_lvgl_port_cm_active());

    vTaskDelete(NULL);                  /* 裁判退场，界面留着给人滑 */
}

void test_t5_colmajor_swipe(void)
{
    /* 栈 1024 字(4KB)：与 task_ui/T3 同级 —— LVGL 渲染链深，2KB 实测溢出过 */
    TEST_ASSERT(xTaskCreate(t5_ui_task, "t5_ui", 1024, NULL, 3, NULL) == pdPASS);
    TEST_ASSERT(xTaskCreate(judge_task, "judge", 256, NULL, 2, NULL) == pdPASS);

    uart_printf("test_t5_colmajor_swipe: UI+judge 已建，3s 自动演示，8s 判定行，之后随便手滑\r\n");
}

#endif /* PSG_TEST_T5_COLMAJOR_SWIPE */
