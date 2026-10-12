/********************************** (C) COPYRIGHT *******************************
* File Name          : main.c
* Author             : WCH
* Version            : V1.0.0
* Date               : 2025/06/06
* Description        : PSG firmware entry.
*********************************************************************************
* Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
* Attention: This software (modified or not) and binary are used for
* microcontroller manufactured by Nanjing Qinheng Microelectronics.
*******************************************************************************/

/*
 *@Note
 * main 只做四件事：时钟/打印口初始化 -> 单元测试钩子 -> app_main -> 启动调度器。
 * 业务代码一律放 App/，不要往 main.c 里堆。
 *
 * Delay_Init/Delay_Us/Delay_Ms 已从 Debug/ 删除（它们会重配 SysTick，
 * 与 FreeRTOS 的 tick 冲突）；任务内延时一律 vTaskDelay()。
 */

#include "debug.h"
#include "FreeRTOS.h"
#include "task.h"
#include "psg_config.h"
#include "app_main.h"
#include "test_runner.h"

/* Global Variable */

int main(void)
{
    /* 时钟与打印口：USART1_TX(PA9) 115200 */
    SystemCoreClockUpdate();
    bsp_uart_debug_init(115200);
    PSG_LOG("SystemClk:%d\r\n", SystemCoreClock);
    PSG_LOG("ChipID:%08x\r\n", DBGMCU_GetCHIPID());

    /* 单元测试：调度器启动前阻塞跑完，结果从串口输出
     * （运行模式在 Config/psg_config.h 的 PSG_MODE 切换，
     *  非单元测试模式下 test_runner_run 是空实现） */
    test_runner_run();

    /* 应用初始化 + 创建任务（App/app_main.c） */
    app_main();

    /* 启动调度器，正常情况下不会返回 */
    vTaskStartScheduler();

    /* 走到这里说明调度器启动失败 */
    uart_printf("scheduler start failed\r\n");
    while (1)
    {
    }
}
