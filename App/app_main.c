/*********************************************************************
 * app_main.c - 应用层入口
 *
 * 当前内容：FreeRTOS 双任务打印示例（原 User/main.c 里的学习 demo）。
 * 后续按功能拆成 task_<name>.c（task_ui / task_acq / task_storage /
 * task_comm ...），app_main 只负责统一初始化和创建任务。
 *********************************************************************/
#include "debug.h"
#include "FreeRTOS.h"
#include "task.h"
#include "psg_config.h"
#include "app_main.h"

/*********************************************************************
 * 任务1：低优先级，每 1s 打印一次计数
 * 任务函数绝不能 return —— 要么死循环，要么 vTaskDelete(NULL) 删除自己
 *********************************************************************/
static void vTask1(void *pvParameters)
{
    uint32_t cnt = 0;

    (void)pvParameters;

    while (1)
    {
        PSG_LOG("Task1 running, count = %d\r\n", cnt++);
        /* 阻塞 1000ms：任务进入阻塞态，CPU 让给其他就绪任务 */
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/*********************************************************************
 * 任务2：优先级比 Task1 高，每 500ms 打印一次
 *********************************************************************/
static void vTask2(void *pvParameters)
{
    (void)pvParameters;

    while (1)
    {
        PSG_LOG("Task2\r\n");
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

/*********************************************************************
 * xTaskCreate 参数：
 *   1. 任务函数
 *   2. 名字（调试用，configMAX_TASK_NAME_LEN=16）
 *   3. 栈大小，单位是"字"(4字节)，256 = 1KB，够 printf 用
 *   4. 传给任务的参数（这里不用）
 *   5. 优先级（0最低，configMAX_PRIORITIES-1=14最高）
 *   6. 任务句柄（这里不用）
 * 返回 pdPASS 表示创建成功；失败基本是 configTOTAL_HEAP_SIZE 不够
 *********************************************************************/
void app_main(void)
{
    if (xTaskCreate(vTask1, "Task1", 256, NULL, 1, NULL) != pdPASS)
    {
        uart_printf("create Task1 failed\r\n");
    }
    if (xTaskCreate(vTask2, "Task2", 256, NULL, 2, NULL) != pdPASS)
    {
        uart_printf("create Task2 failed\r\n");
    }
}
