/*********************************************************************
 * task_ui.c - UI 任务（检查点 B）
 *
 * 职责 = LVGL 最小生命维持：
 *   1. 移植层初始化（屏 + LVGL + 异步 DMA + 信号量，只做一次）
 *   2. lv_tick_set_cb：把 FreeRTOS 的 tick 换算成 LVGL 的毫秒
 *      （不用 tick hook，一个回调解决，且不会重复计时）
 *   3. 建静态页面（与单元测试同款：红字 Hello 居中）
 *   4. 主循环跑 lv_timer_handler：LVGL 9.6 返回"建议等待 ms"，
 *      夹到 [1,10]ms 后 vTaskDelay —— 任务进 Blocked 态，CPU 让出
 *
 * DMA 等待期间本任务阻塞在信号量上（flush_wait_cb），Task1/Task2
 * 照常打印 —— 这就是"UI 刷屏不卡别人"的活体证据
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

void task_ui(void *arg)
{
    TickType_t last_stat = xTaskGetTickCount();

    (void)arg;

    /* 1. 移植层：LCD + LVGL（横屏 320x240）+ 异步 DMA 三件套 */
    bsp_lvgl_port_init(LCD_ROT_LANDSCAPE);
    bsp_lvgl_port_async_enable();   /* 信号量 + DMA 完成中断 + wait_cb（B 档） */

    /* 2. 唯一 tick 源：替代 lv_tick_inc 方案，防重复计时 */
    lv_tick_set_cb(lv_tick_cb);

    /* 3. 静态页面：与单元测试同款视觉（红字=颜色诊断沿用） */
    lv_obj_t *label = lv_label_create(lv_screen_active());
    lv_label_set_text(label, "Hello LVGL11!");
    lv_obj_set_style_text_color(label, lv_color_hex(0xFF0000), 0);
    lv_obj_center(label);

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

            bsp_lcd_dma_stats(&ds, &dd, &de);
            /* 栈水位 = 历史最小剩余（字）：改控件/换字体后重看这个数，
             * 余量 < 1/3 就该加栈 —— 量，别猜（2KB 溢出的教训） */
            uart_printf("task_ui stat: flush=%lu dma %lu/%lu err=%lu stk_free=%lu w\r\n",
                        (unsigned long)bsp_lvgl_port_flush_cnt(),
                        (unsigned long)ds, (unsigned long)dd,
                        (unsigned long)de,
                        (unsigned long)uxTaskGetStackHighWaterMark(NULL));
            last_stat = xTaskGetTickCount();
        }
    }
}
