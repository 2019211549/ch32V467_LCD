/********************************** (C) COPYRIGHT *******************************
* File Name          : freertos_hooks.h
* Description        : FreeRTOS 应用层钩子函数声明。
*                      configSUPPORT_STATIC_ALLOCATION = 1 时，内核内部的
*                      空闲任务 / 定时器任务改为静态创建，其 TCB 和栈内存
*                      必须由应用通过下列钩子函数提供（实现见 freertos_hooks.c）。
*********************************************************************************/

#ifndef __FREERTOS_HOOKS_H
#define __FREERTOS_HOOKS_H

#ifdef __cplusplus
extern "C" {
#endif

#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"

#if( configSUPPORT_STATIC_ALLOCATION == 1 )
/* 空闲任务内存钩子：vTaskStartScheduler() 内部创建空闲任务时回调 */
void vApplicationGetIdleTaskMemory( StaticTask_t **ppxIdleTaskTCBBuffer,
                                    StackType_t **ppxIdleTaskStackBuffer,
                                    uint32_t *pulIdleTaskStackSize );
#endif

#if( ( configUSE_TIMERS == 1 ) && ( configSUPPORT_STATIC_ALLOCATION == 1 ) )
/* 定时器服务任务内存钩子：xTimerCreateTimerTask() 创建定时器任务时回调 */
void vApplicationGetTimerTaskMemory( StaticTask_t **ppxTimerTaskTCBBuffer,
                                     StackType_t **ppxTimerTaskStackBuffer,
                                     uint32_t *pulTimerTaskStackSize );
#endif

#ifdef __cplusplus
}
#endif

#endif /* __FREERTOS_HOOKS_H */
