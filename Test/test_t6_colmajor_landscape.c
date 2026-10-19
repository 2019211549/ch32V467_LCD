/*********************************************************************
 * test_t6_colmajor_landscape.c - T6 用例：列优先横屏直通滑动切页
 *
 * T6 = T5 的物理底座 + 横屏正向 UI，两者兼得的关键是 LVGL 内核补丁
 * （LV_USE_COLMAJOR_RENDER，见 Config/lv_conf.h 与 lv_refr.c）：
 *   PARTIAL 刷新从"按行切全宽横条"改成"按列切全高竖条"（x 升序）→
 *   横屏 320x240 声明下，每条脏区物理上就是 16 列 x 全高竖板。
 *   flush_cb 流水（方案定稿双车道）：转置(行主序→列主序) → [周期首条：
 *   排干上周期末条 DMA + GSCAN 回绕对齐 δ0=0] → 交换参数窗口(CASET=v
 *   全高/RASET=u 板宽) → 乒乓缓冲一笔异步 DMA → 转置完立即 flush_ready
 *   （画布马上回 LVGL 渲染下一板 = 发送/渲染并行）。
 *   撕裂同步物理与 T5 全同（板证：写前沿领先追扫 → 同帧翻色 ≈60fps），
 *   增量成本只有每板 ~25-50µs 转置 + 240 短 span 渲染开销（gap 统计盯）。
 *
 * 本课验收点（对照 T5 的"文字横躺 90°+镜像"）：
 *   1. 文字/控件全部正向不镜像 —— "F" 字标 + Page 文本 + OK 按钮
 *   2. 四角色标：左上白/右上黄/左下青/右下品红（转置若含意外翻转立刻现形）
 *   3. 滑动无撕裂线 + 手感顺滑（同 T5）；串口 gap avg<~42µs（19 板 Σgap
 *      <800µs 判据）说明渲染+转置藏进了 819µs DMA 影子
 *
 * 两段式（同 T3/T5）：3s 自动演示匀速滑一页（500ms），6s 校验
 *   scroll_x==320，8s judge 判定行（含 cmlm 帧/周期/sync_miss/转置/间隙），
 *   之后界面常驻随便手滑。
 *
 * 互斥：与 T3/T4/T5（同款 UI 任务抢渲染）、PSG_TEST_BSP_TOUCH（抢 I3C）。
 *********************************************************************/
#include "test_config.h"   /* 开关必须先包含再 #if，写反整文件编译为空 */

#if PSG_TEST_T6_COLMAJOR_LM

#include "FreeRTOS.h"
#include "task.h"
#include "lvgl.h"
#include "test_framework.h"
#include "bsp_lvgl_port.h"
#include "bsp_lcd.h"       /* bsp_lcd_dma_stats / bsp_lcd_dma_fallback_cnt */
#include "bsp_uart.h"

#define T6_PAGE_W       320    /* 横屏页宽 = 玻璃长边（滑一页 = 320px） */
#define T6_PAGE_H       240    /* 横屏页高 = 玻璃短边 */
#define T6_PAGE_CNT     3

static const uint32_t s_page_color[T6_PAGE_CNT] = {
    0xE23B3B,      /* 页1 红（兼作 BGR/INVON 颜色诊断：红=全对） */
    0x3B82E8,      /* 页2 蓝 */
    0x2FBF71,      /* 页3 绿 */
};

static lv_obj_t *s_cont;               /* snap 容器（自动演示/校验要访问） */
static volatile uint8_t s_touch_ok;    /* t6_ui 记录，judge 读 */
static volatile uint8_t s_scroll_ok;   /* 程序化 snap 是否精确到位 */

/* LVGL 毫秒时基（与 task_ui/T5 同款；各文件 static，不建公共文件） */
static uint32_t t6_tick_cb(void)
{
    return (uint32_t)xTaskGetTickCount() * portTICK_PERIOD_MS;
}

/* 自建匀速翻页动画（同 T3/T5 的撕裂优化理由）：LVGL 默认 scroll 动画是
 * ease_out，起步一帧内容跳 ~80px —— 错位最宽的瞬间最显裂痕；改 500ms
 * 匀速（峰值=平均速度，16ms 帧周期下一帧步进恒定 ~13px），滑进运动模糊 */
static void t6_scroll_exec_cb(void *obj, int32_t v)
{
    lv_obj_scroll_to_x((lv_obj_t *)obj, v, LV_ANIM_OFF);  /* 每步直接落位 */
}

/* 自动演示 + 校验（跑在 t6_ui 的 lv_timer 上下文）：
 * 第一次到期（3s）：程序化滑到第 2 页（带动画）
 * 第二次到期（6s）：读 scroll_x 校验吸附精度，然后自删 —— 之后全靠手 */
static void t6_auto_cb(lv_timer_t *timer)
{
    static uint8_t step;

    if (step == 0)
    {
        lv_anim_t a;

        lv_anim_init(&a);
        lv_anim_set_var(&a, s_cont);
        lv_anim_set_values(&a, 0, T6_PAGE_W);      /* 页1 → 页2 */
        lv_anim_set_duration(&a, 500);             /* 500ms 匀速 = 一帧步进 ~13px */
        lv_anim_set_path_cb(&a, lv_anim_path_linear);
        lv_anim_set_exec_cb(&a, t6_scroll_exec_cb);
        lv_anim_start(&a);
        step = 1;
    }
    else
    {
        int32_t sx = lv_obj_get_scroll_x(s_cont);

        s_scroll_ok = (sx == T6_PAGE_W) ? 1 : 0;
        uart_printf("t6: auto-snap scroll_x=%d (期望 %d)\r\n", (int)sx, T6_PAGE_W);
        lv_timer_delete(timer);
    }
}

/* 四角色标：12x12 色块。转置/翻转若有差错，四角颜色对不上号立刻现形 */
static void t6_add_mark(lv_obj_t *parent, uint32_t color, int32_t x, int32_t y)
{
    lv_obj_t *m = lv_obj_create(parent);

    lv_obj_set_size(m, 12, 12);
    lv_obj_set_pos(m, x, y);
    lv_obj_set_style_bg_color(m, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(m, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(m, 0, 0);
    lv_obj_set_style_radius(m, 0, 0);
    lv_obj_clear_flag(m, LV_OBJ_FLAG_SCROLLABLE);
}

/* "F" 字标（方向判据：F 正立且笔画朝右 = 无旋转无镜像；T5 时代它是
 * 横躺+镜像的）。白底黑字框，任何页面底色上都醒目 */
static void t6_add_fmark(lv_obj_t *parent, int32_t x, int32_t y)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_t *lbl = lv_label_create(box);

    lv_obj_set_size(box, 36, 36);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_style_bg_color(box, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_radius(box, 4, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_label_set_text(lbl, "F");
    lv_obj_set_style_text_color(lbl, lv_color_hex(0x000000), 0);
    lv_obj_center(lbl);
}

static void t6_ui_task(void *arg)
{
    TickType_t last_stat = xTaskGetTickCount();
    uint8_t cml_rc;
    uint8_t touch_rc;
    uint8_t i;

    (void)arg;

    /* T6 底座（无参数）：横屏 320x240 声明 + 列优先内核补丁 + 转置/乒乓
     * DMA + GSCAN 回绕对齐。注意：本模式不要调 bsp_lvgl_port_async_enable */
    cml_rc = bsp_lvgl_port_init_colmajor_landscape();
    if (cml_rc != 0)
    {
        uart_printf("t6: 列优先横屏 init rc=%d（本次上电已被其它模式初始化？）\r\n",
                    (int)cml_rc);
    }
    lv_tick_set_cb(t6_tick_cb);

    /* 触摸：横屏默认映射（T5 开过竖屏档的话这里显式切回） */
    bsp_lvgl_port_touch_set_portrait(0);
    touch_rc = bsp_lvgl_port_touch_init();
    s_touch_ok = (touch_rc == 0) ? 1 : 0;
    if (touch_rc != 0)
    {
        uart_printf("t6: touch init fail rc=%d（只能看自动演示，手滑无效）\r\n",
                    (int)touch_rc);
    }

    /* 页面：一个 screen + 横排 snap 容器 + 3 整屏彩色页 —— 与 T3 同款
     * 横屏坐标，区别只在底座：这里文字是正向的（T6 的全部意义） */
    s_cont = lv_obj_create(lv_screen_active());
    lv_obj_set_size(s_cont, T6_PAGE_W, T6_PAGE_H);
    lv_obj_center(s_cont);
    lv_obj_set_flex_flow(s_cont, LV_FLEX_FLOW_ROW);       /* 横向拼接 */
    lv_obj_set_style_pad_all(s_cont, 0, 0);               /* 无缝拼接 */
    lv_obj_set_style_border_width(s_cont, 0, 0);
    lv_obj_set_scroll_dir(s_cont, LV_DIR_HOR);
    lv_obj_set_scroll_snap_x(s_cont, LV_SCROLL_SNAP_CENTER); /* 松手页对中 */
    lv_obj_set_scrollbar_mode(s_cont, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_anim_duration(s_cont, 400, 0);       /* snap 回位动画 */

    for (i = 0; i < T6_PAGE_CNT; i++)
    {
        lv_obj_t *pg  = lv_obj_create(s_cont);
        lv_obj_t *lbl = lv_label_create(pg);

        lv_obj_set_size(pg, T6_PAGE_W, T6_PAGE_H);
        lv_obj_set_style_bg_color(pg, lv_color_hex(s_page_color[i]), 0);
        lv_obj_set_style_border_width(pg, 0, 0);
        lv_obj_set_style_pad_all(pg, 0, 0);
        lv_obj_set_scrollbar_mode(pg, LV_SCROLLBAR_MODE_OFF);
        lv_obj_clear_flag(pg, LV_OBJ_FLAG_SCROLLABLE);    /* 页自身不可滚 */

        /* 方向验证三件套：F 字标 + 四角色标 + 页码文本（全正向） */
        t6_add_fmark(pg, 60, 40);
        t6_add_mark(pg, 0xFFFFFF, 0,   0);    /* 左上 白 */
        t6_add_mark(pg, 0xFFD400, 308, 0);    /* 右上 黄 */
        t6_add_mark(pg, 0x00E5FF, 0,   228);  /* 左下 青 */
        t6_add_mark(pg, 0xE040FB, 308, 228);  /* 右下 品红 */

        lv_label_set_text_fmt(lbl, "Page %d/3", (int)(i + 1));  /* 正向文本 */
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
        lv_obj_center(lbl);

        if (i == 0)    /* 常规控件抽查：按钮（主题圆角+按压态，横屏正向） */
        {
            lv_obj_t *btn  = lv_button_create(pg);
            lv_obj_t *btxt = lv_label_create(btn);

            lv_obj_set_pos(btn, 120, 170);
            lv_obj_set_size(btn, 80, 40);
            lv_label_set_text(btxt, "OK");
            lv_obj_center(btxt);
        }
    }

    /* 临时诊断：UI 建完立即体检池子（HardFault 定位用） */
    {
        lv_mem_monitor_t mon;
        extern void dbg_lv_snapshot(char tag);

        lv_mem_monitor(&mon);
        uart_printf("t6: UI built 池=%d free=%lu\r\n",
                    (int)lv_mem_test(), (unsigned long)mon.free_size);
        dbg_lv_snapshot('B');
    }

    /* 3s 自动演示，6s 校验 */
    lv_timer_create(t6_auto_cb, 3000, NULL);

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

        /* 10s 统计：滑动期间 flush 增速=实际帧率（全屏滚动一帧 = 20 板）；
         * gap avg 是流水判据 —— 19 板 Σgap<800µs 即 avg<~42µs */
        if ((xTaskGetTickCount() - last_stat) >= pdMS_TO_TICKS(10000))
        {
            uint32_t ds = 0, dd = 0, de = 0, terr = 0;

            bsp_lcd_dma_stats(&ds, &dd, &de);
            bsp_lvgl_port_touch_dbg(0, 0, 0, 0, 0, &terr);
            uart_printf("t6 stat: flush=%lu dma %lu/%lu err=%lu fb=%lu t_err=%lu stk_free=%lu w\r\n",
                        (unsigned long)bsp_lvgl_port_flush_cnt(),
                        (unsigned long)ds, (unsigned long)dd,
                        (unsigned long)de,
                        (unsigned long)bsp_lcd_dma_fallback_cnt(),
                        (unsigned long)terr,
                        (unsigned long)uxTaskGetStackHighWaterMark(NULL));
            if (bsp_lvgl_port_colmajor_lm_active() != 0)
            {
                uint32_t fr = 0, avg = 0, mx = 0, miss = 0;
                uint32_t trm = 0, ga = 0, gm = 0;

                bsp_lvgl_port_colmajor_lm_stats(&fr, &avg, &mx, &miss, &trm, &ga, &gm);
                uart_printf("t6 stat: 列横 帧%lu 周期avg %lums max %lums sync_miss=%lu "
                            "转置max %luus 间隙avg/max %lu/%luus\r\n",
                            (unsigned long)fr, (unsigned long)(avg / 1000u),
                            (unsigned long)(mx / 1000u), (unsigned long)miss,
                            (unsigned long)trm, (unsigned long)ga, (unsigned long)gm);
            }
            last_stat = xTaskGetTickCount();
        }
    }
}

/* 裁判：8s 收数（自动 snap 已完成、校验已跑），判定行晚于 runner
 * 汇总行出现属正常（同 T3/T5） */
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

    TEST_ASSERT(flush >= 10);           /* 至少刷满一整帧（20 板） */
    TEST_ASSERT(s_touch_ok != 0);       /* 触摸在线（手滑路径可用） */
    TEST_ASSERT(s_scroll_ok != 0);      /* 程序化 snap 精确到位 */
    TEST_ASSERT_EQ_INT(start, done);    /* DMA 每笔有始有终 */
    TEST_ASSERT_EQ_INT(err, 0);
    TEST_ASSERT_EQ_INT(bsp_lcd_dma_fallback_cnt(), 0);

    /* T6 专属判定（模式没生效时 lm_active=0，整段跳过）：帧数/回绕同步
     * 同 T5；gap avg 是本课新指标 —— <~42µs 说明渲染+转置藏进 819µs DMA
     * 影子（>819µs 则流水退化成串行，帧率掉但显示仍正确，靠眼睛+串口定） */
    if (bsp_lvgl_port_colmajor_lm_active() != 0)
    {
        uint32_t fr = 0, avg = 0, mx = 0, miss = 0;
        uint32_t trm = 0, ga = 0, gm = 0;

        bsp_lvgl_port_colmajor_lm_stats(&fr, &avg, &mx, &miss, &trm, &ga, &gm);
        TEST_ASSERT(fr >= 5);
        TEST_ASSERT_EQ_INT(0, (int)miss);

        uart_printf("t6 cmlm(judge@8s): 帧%lu 周期avg %lums max %lums sync_miss=%lu "
                    "转置max %luus 间隙avg/max %lu/%luus —— F正向+四角色标对号+"
                    "滑动无撕裂线 即收官\r\n",
                    (unsigned long)fr, (unsigned long)(avg / 1000u),
                    (unsigned long)(mx / 1000u), (unsigned long)miss,
                    (unsigned long)trm, (unsigned long)ga, (unsigned long)gm);
    }

    uart_printf("[%s] test_t6_landscape(judge@8s): flush=%lu snap=%d touch=%d dma %lu/%lu err=%lu fb=%lu lm=%d\r\n",
                (test_fail_cnt == fail0) ? " OK " : "FAIL",
                (unsigned long)flush,
                (int)s_scroll_ok, (int)s_touch_ok,
                (unsigned long)start, (unsigned long)done,
                (unsigned long)err,
                (unsigned long)bsp_lcd_dma_fallback_cnt(),
                (int)bsp_lvgl_port_colmajor_lm_active());

    vTaskDelete(NULL);                  /* 裁判退场，界面留着给人滑 */
}

void test_t6_colmajor_landscape(void)
{
    /* 栈 1024 字(4KB)：与 task_ui/T3/T5 同级 —— LVGL 渲染链深，2KB 溢出过 */
    TEST_ASSERT(xTaskCreate(t6_ui_task, "t6_ui", 1024, NULL, 3, NULL) == pdPASS);
    TEST_ASSERT(xTaskCreate(judge_task, "judge", 256, NULL, 2, NULL) == pdPASS);

    uart_printf("test_t6_colmajor_landscape: UI+judge 已建，3s 自动演示，8s 判定行，之后随便手滑\r\n");
}

#endif /* PSG_TEST_T6_COLMAJOR_LM */
