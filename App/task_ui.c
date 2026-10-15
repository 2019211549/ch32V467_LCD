/*********************************************************************
 * task_ui.c - UI 任务（检查点 B/C）
 *
 * 职责 = LVGL 最小生命维持：
 *   1. 移植层初始化（屏 + LVGL + 异步 DMA + 信号量，只做一次）
 *   2. lv_tick_set_cb：把 FreeRTOS 的 tick 换算成 LVGL 的毫秒
 *      （不用 tick hook，一个回调解决，且不会重复计时）
 *   3. 页面：红字 Hello 居中 + 每秒计数 Label（C：持续局部刷新源）
 *   4. 主循环跑 lv_timer_handler：LVGL 9.6 返回"建议等待 ms"，
 *      夹到 [1,10]ms 后 vTaskDelay —— 任务进 Blocked 态，CPU 让出
 *
 * DMA 等待期间本任务阻塞在信号量上（flush_wait_cb），低优先级任务
 * 照常跑 —— 这就是"UI 刷屏不卡别人"的活体证据
 *********************************************************************/
#include "FreeRTOS.h"
#include "task.h"
#include "lvgl.h"
#include "bsp_lvgl_port.h"
#include "bsp_lcd.h"
#include "bsp_uart.h"

/* LVGL 的毫秒时基：FreeRTOS tick 500Hz -> 每 tick 2ms。
 * xTaskGetTickCount() 回绕约 24.8 天，LVGL 内部全部用差值计时，安全 */
static uint32_t lv_tick_cb(void)
{
    return (uint32_t)xTaskGetTickCount() * portTICK_PERIOD_MS;
}

/* 计数定时器回调（lv_timer_handler 上下文，即本任务里，可安全调 LVGL）：
 * 只改文字 -> 只弄脏这个 Label 的矩形 -> 下一轮只重刷那一小块。
 * lv_label_set_text_fmt 用 LVGL 内置 sprintf（支持 %d，无 %f 问题）。
 * 注意：9.6 的 lv_timer_t 是不透明类型，user_data 必须用访问器取 */
static void cnt_timer_cb(lv_timer_t *timer)
{
    static uint32_t n;

    n++;
    lv_label_set_text_fmt((lv_obj_t *)lv_timer_get_user_data(timer),
                          "cnt: %d", (int)n);
}

/* 按钮 CLICKED 事件（T2b 触摸验收控件）：点击事件由 LVGL 从 indev 的
 * 按下/抬起状态变化自动合成 —— 这里只数数改字，什么都不用判 */
static void btn_event_cb(lv_event_t *e)
{
    static uint32_t taps;

    lv_label_set_text_fmt((lv_obj_t *)lv_event_get_user_data(e),
                          "Tap: %d", (int)(++taps));
}

/* 触摸日志（T2a 映射数字验证）：从 indev 真链路取最近一次读数，
 * 只在按下/抬起边沿各打一行 —— 摸屏幕四角看 raw→map 是否落位 */
static void touch_log_cb(lv_timer_t *timer)
{
    static uint8_t last;
    uint16_t gx = 0, gy = 0, u = 0, v = 0;
    uint8_t p = 0;
    uint32_t err = 0;

    (void)timer;
    bsp_lvgl_port_touch_dbg(&gx, &gy, &u, &v, &p, &err);
    if (p != last)
    {
        uart_printf("touch: raw(%d,%d) -> map(%d,%d) %s err=%lu\r\n",
                    (int)gx, (int)gy, (int)u, (int)v,
                    (p != 0) ? "PRESS" : "RELEASE", (unsigned long)err);
        last = p;
    }
}

void task_ui(void *arg)
{
    TickType_t last_stat = xTaskGetTickCount();  // 上一次统计时间
    uint8_t touch_rc; //触摸初始化结果

    (void)arg;

    /* 1. 移植层：LCD + LVGL（横屏 320x240）+ 异步 DMA 三件套 */
    bsp_lvgl_port_init(LCD_ROT_LANDSCAPE);
    bsp_lvgl_port_async_enable();   /* 信号量 + DMA 完成中断 + wait_cb（B 档） */

    /* 2. 唯一 tick 源：替代 lv_tick_inc 方案，防重复计时 */
    lv_tick_set_cb(lv_tick_cb);

    /* 3. 触摸 indev（T2）：内部幂等；不在线不挡显示，只打一行 */
    touch_rc = bsp_lvgl_port_touch_init();
    if (touch_rc != 0)
    {
        uart_printf("task_ui: touch init fail rc=%d（继续跑显示）\r\n", (int)touch_rc);
    }

    /* 4. 页面：红字 Hello（顶部，颜色诊断沿用）+ 按钮（T2b 触摸验收：
     *    手指点它计数+1 = 触摸→indev→事件→UI 全链路通）+ 每秒计数 Label
     *    （底部，检查点 C：局部刷新的活体演示） */
    lv_obj_t *label = lv_label_create(lv_screen_active());
    lv_label_set_text(label, "Hello LVGL11!");
    lv_obj_set_style_text_color(label, lv_color_hex(0xFF0000), 0);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 15);

    lv_obj_t *btn = lv_button_create(lv_screen_active());
    lv_obj_set_size(btn, 160, 60);
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, 0);
    lv_obj_t *btn_lbl = lv_label_create(btn);       /* 按钮内建 Label 显示计数 */
    lv_label_set_text(btn_lbl, "Tap: 0");
    lv_obj_center(btn_lbl);
    lv_obj_add_event_cb(btn, btn_event_cb, LV_EVENT_CLICKED, btn_lbl);

    lv_obj_t *cnt_lbl = lv_label_create(lv_screen_active());
    lv_label_set_text(cnt_lbl, "cnt: 0");
    lv_obj_set_style_text_color(cnt_lbl, lv_color_hex(0xFF0000), 0);
    lv_obj_align(cnt_lbl, LV_ALIGN_BOTTOM_MID, 0, -15);
    lv_timer_create(cnt_timer_cb, 1000, cnt_lbl);   /* 1s 一跳，回调里只改文字 */

    if (touch_rc == 0)
    {
        lv_timer_create(touch_log_cb, 100, NULL);   /* T2a：边沿触发打映射数字 */
    }

    /* 4. 心跳循环 */
    for (;;)
    {
        uint32_t wait = lv_timer_handler();     /* 内部可能阻塞等 DMA 完成 */

        if (wait == 0)
        {
            wait = 1;                           /* 0 也睡 1 个 tick，防忙转 */
        }
        if (wait > 10)
        {
            wait = 10;                          /* 上限 10ms：动画/触摸响应不吃亏 */
        }
        vTaskDelay(pdMS_TO_TICKS(wait));        /* Blocked 态，CPU 让出 */

        /* 每 5s 打印一次统计（检查点 B 判据：flush 持续增长，
         * dma start==done 持续成立，err==0；Task1/2 节律不受影响） */
        if ((xTaskGetTickCount() - last_stat) >= pdMS_TO_TICKS(5000))
        {
            uint32_t ds = 0, dd = 0, de = 0;
            uint32_t terr = 0;

            bsp_lcd_dma_stats(&ds, &dd, &de);
            bsp_lvgl_port_touch_dbg(0, 0, 0, 0, 0, &terr);  /* 触摸长期健康指标 */
            /* 栈水位 = 历史最小剩余（字）：改控件/换字体后重看这个数，
             * 余量 < 1/3 就该加栈 —— 量，别猜（2KB 溢出的教训） */
            uart_printf("task_ui stat: flush=%lu dma %lu/%lu err=%lu fb=%lu t_err=%lu stk_free=%lu w\r\n",
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
