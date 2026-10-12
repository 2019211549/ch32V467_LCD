/********************************** (C) COPYRIGHT *******************************
* File Name          : ch32v4x7_it.c
* Author             : WCH
* Version            : V1.0.0
* Date               : 2025/12/01
* Description        : Main Interrupt Service Routines.
*********************************************************************************
* Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
* Attention: This software (modified or not) and binary are used for 
* microcontroller manufactured by Nanjing Qinheng Microelectronics.
*******************************************************************************/
#include "ch32v4x7_it.h"
#include "bsp_lcd.h"
#include "bsp_uart.h"

/*********************************************************************
 * 检查点 B：SPI1 TX DMA（DMA1_Channel3）传输完成中断。
 * 属性必须是 WCH-Interrupt-fast（HPE 硬件压栈使能时，普通 interrupt
 * 属性不保存全部寄存器）。服务体实现在 bsp_lcd.c
 *********************************************************************/
void DMA1_Channel3_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void DMA1_Channel3_IRQHandler(void)
{
    bsp_lcd_dma_irq_handler();
}

void NMI_Handler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void HardFault_Handler(void) __attribute__((interrupt("WCH-Interrupt-fast")));

/*********************************************************************
 * @fn      NMI_Handler
 *
 * @brief   This function handles NMI exception.
 *
 * @return  none
 */
void NMI_Handler(void)
{
  while (1)
  {
  }
}

/*********************************************************************
 * @fn      HardFault_Handler
 *
 * @brief   This function handles Hard Fault exception.
 *
 * @return  none
 */
/*********************************************************************
 * 检查点 B 排障（临时）：原来直接 NVIC_SystemReset() 无声重启，
 * 复位循环什么线索都不留。改为打印异常现场后挂起：
 *   mcause: bit31=1 中断 / 0 异常，低 5 位为编号（如 2=非法指令 4=加载错 5=存储错）
 *   mepc  : 出错 PC —— 对照 .map / lst 反查函数
 * 定位完成后恢复成 NVIC_SystemReset()
 *********************************************************************/
void HardFault_Handler(void)
{
  uart_printf("HardFault: mcause=0x%x mepc=0x%x\r\n",
              (unsigned)__get_MCAUSE(), (unsigned)__get_MEPC());
  while (1)
  {
  }
}


