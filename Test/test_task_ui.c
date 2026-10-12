/*********************************************************************
 * test_task_ui.c - 检查点 B 用例：调度器里的 UI 任务（异步 DMA + 信号量）
 *
 * 与其它用例的本质区别：要验证的行为发生在 vTaskStartScheduler 之后
 * （task_ui 阻塞在信号量上、被 DMA 完成中断唤醒），而 test_runner_run
 * 跑在调度器启动之前。所以分两段：
 *
 *   调度器前 test_task_ui()：布置考场 —— 创建 task_ui（被测对象）和
 *     judge 裁判任务。此时 TEST_RUN 打的 [ OK ] 只代表"任务创建成功"。
 *
 *   调度器后 judge_task()：睡 6s（task_ui 完成首帧、打完第一轮统计），
 *     对 flush / dma start==done / err==0 做断言，打印本用例真正的
 *     判定行 —— 晚于 runner 汇总行出现，属正常，以判定行为准。
 *
 * 断言宏 = 计数器 + 打印（test_framework.h），任务上下文可直接用。
 *********************************************************************/
#include "test_config.h"   /* PSG_TEST_TASK_UI 开关：必须先包含再 #if，
                            * 写反了宏未定义 -> 整个文件编译为空 -> 链接报
                            * undefined reference（2026-10 踩过） */

#if PSG_TEST_TASK_UI

#include "FreeRTOS.h"
#include "task.h"
#include "test_framework.h"
#include "task_ui.h"
#include "bsp_lvgl_port.h"
#include "bsp_lcd.h"
#include "bsp_uart.h"

/* 裁判任务：6s 后收数断言。
 * 计数是全局累计的（hello 等更早用例的 DMA 也在里面），所以只断言
 * "至少一整帧"（横屏 320x240 / 24 行 = 10 块）而不是精确值 */
static void judge_task(void *arg)
{
    uint32_t start = 0, done = 0, err = 0;
    uint32_t fail0;
    uint32_t flush;

    (void)arg;

    vTaskDelay(pdMS_TO_TICKS(6000));    /* 等首帧 + 第一轮 5s 统计打完 */

    bsp_lcd_dma_stats(&start, &done, &err);
    flush = bsp_lvgl_port_flush_cnt();
    fail0 = test_fail_cnt;              /* 判定行只看本段的失败增量 */

    TEST_ASSERT(flush >= 10);           /* 至少刷满一整帧 */
    TEST_ASSERT_EQ_INT(start, done);    /* DMA 每笔都有始有终（没卡在半路） */
    TEST_ASSERT_EQ_INT(err, 0);         /* 无超时、无强制恢复 */
    TEST_ASSERT_EQ_INT(bsp_lcd_dma_fallback_cnt(), 0);  /* 不变量：始终走异步，一次没降级过 */

    uart_printf("[%s] test_task_ui(judge@6s): flush=%lu dma %lu/%lu err=%lu fb=%lu\r\n",
                (test_fail_cnt == fail0) ? " OK " : "FAIL",
                (unsigned long)flush,
                (unsigned long)start, (unsigned long)done,
                (unsigned long)err,
                (unsigned long)bsp_lcd_dma_fallback_cnt());

    vTaskDelete(NULL);                  /* 裁判退场，别占调度表 */
}

void test_task_ui(void)
{
    /* 栈 1024 字(4KB)：LVGL 渲染链深，512 字(2KB) 实测溢出 ->
     * 踩坏 FreeRTOS 堆 -> HardFault 复位循环（2026-10 排障结论） */
    TEST_ASSERT(xTaskCreate(task_ui, "task_ui", 1024, NULL, 3, NULL) == pdPASS);

    /* 裁判：优先级 2（低于被测的 task_ui=3，它刷屏时裁判不添乱），
     * 栈 256 字够打印用 */
    TEST_ASSERT(xTaskCreate(judge_task, "judge", 256, NULL, 2, NULL) == pdPASS);

    uart_printf("test_task_ui: UI+judge 已建，判定行在调度器启动 6s 后打印\r\n");
}

#endif /* PSG_TEST_TASK_UI */
