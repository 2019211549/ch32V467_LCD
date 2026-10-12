/********************************** (C) COPYRIGHT *******************************
* File Name          : freertos_hooks.c
* Description        : FreeRTOS 应用层钩子函数实现。
*
* 背景：FreeRTOSConfig.h 中 configSUPPORT_STATIC_ALLOCATION = 1 时，
*       内核自己的两个内部任务（空闲任务、定时器服务任务）也改为静态
*       创建。静态创建 = 内存由应用提供，但内核代码够不到应用的变量，
*       所以约定两个固定名字的钩子函数，内核通过它们向应用"要内存"。
*       这两个函数只被内核调用，应用不需要主动调用。
*
* 注意：这里只给内核内部任务供内存。你自己的静态任务（xTaskCreateStatic）
*       的 TCB / 栈数组仍然写在各自任务的 .c 里。
*********************************************************************************/

#include "freertos_hooks.h"
#include "bsp_uart.h"

#if( configSUPPORT_STATIC_ALLOCATION == 1 )

/*********************************************************************
 * 空闲任务（Idle Task）内存
 * - 调度器一启动（vTaskStartScheduler）就创建，优先级 0（最低），
 *   没有其他任务可运行时靠它兜底（通常进入低功耗）。
 * - 栈大小取 configMINIMAL_STACK_SIZE = 256 字（RV32 下 1 字 = 4 字节）。
 *********************************************************************/
static StaticTask_t xIdleTaskTCB;  //定义了一块与真实的tcb头文件一样的内存指针
static StackType_t  xIdleTaskStack[configMINIMAL_STACK_SIZE]; // 同理是任务帧指针

void vApplicationGetIdleTaskMemory( StaticTask_t **ppxIdleTaskTCBBuffer,
                                    StackType_t **ppxIdleTaskStackBuffer,
                                    uint32_t *pulIdleTaskStackSize )
{
    *ppxIdleTaskTCBBuffer   = &xIdleTaskTCB;
    *ppxIdleTaskStackBuffer = xIdleTaskStack;
    *pulIdleTaskStackSize   = configMINIMAL_STACK_SIZE;
}

#if( configUSE_TIMERS == 1 )
/*********************************************************************
 * 定时器服务任务（Timer/daemon Task）内存
 * - configUSE_TIMERS = 1 时内核创建；所有软件定时器的回调函数
 *   都在这个任务的上下文里执行。
 * - 栈大小取 configTIMER_TASK_STACK_DEPTH（当前 = configMINIMAL_STACK_SIZE）。
 *   若定时器回调里调用较深的函数（如 printf），可单独调大
 *   FreeRTOSConfig.h 里这个宏。
 *********************************************************************/
static StaticTask_t xTimerTaskTCB;
static StackType_t  xTimerTaskStack[configTIMER_TASK_STACK_DEPTH];

void vApplicationGetTimerTaskMemory( StaticTask_t **ppxTimerTaskTCBBuffer,
                                     StackType_t **ppxTimerTaskStackBuffer,
                                     uint32_t *pulTimerTaskStackSize )
{
    *ppxTimerTaskTCBBuffer   = &xTimerTaskTCB;
    *ppxTimerTaskStackBuffer = xTimerTaskStack;
    *pulTimerTaskStackSize   = configTIMER_TASK_STACK_DEPTH;
}
#endif /* configUSE_TIMERS */

#endif /* configSUPPORT_STATIC_ALLOCATION */

/*********************************************************************
 * 栈溢出钩子（configCHECK_FOR_STACK_OVERFLOW = 2 时由内核调用）
 * 方法 2 = 任务栈尾画"脏图案"，上下文切换时校验；溢出刚发生就能抓到，
 * 比溢出后踩坏堆 → 无声 HardFault 复位循环好定位得多。
 * 注意：钩子被调用时那个任务的状态已不可信，打印后挂起等人工看。
 *********************************************************************/
#if( configCHECK_FOR_STACK_OVERFLOW >= 1 )
void vApplicationStackOverflowHook( TaskHandle_t xTask, char *pcTaskName )
{
    (void)xTask;
    uart_printf("[STACK OVFL] task=%s\r\n", pcTaskName);
    while (1) { }
}
#endif
