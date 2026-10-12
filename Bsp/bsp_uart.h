/*********************************************************************
 * bsp_uart.h - 调试串口打印（自实现，不依赖 libc 的 printf）
 *
 * 输出口跟随 Debug/debug.h 的 DEBUG 宏（默认 USART1_TX PA9）。
 *********************************************************************/
#ifndef BSP_UART_H
#define BSP_UART_H

#include <stdint.h>

/* 初始化调试串口（内部复用 vendor 的 USART_Printf_Init） */
void bsp_uart_debug_init(uint32_t baudrate);

/* 裸发送：阻塞发送 len 字节 */
void uart_send(const char *buf, uint32_t len);

/* 发送 '\0' 结尾字符串 */
void uart_puts(const char *s);

/* 自实现格式化打印。支持：%d %i %u %x %X %p %c %s %%
 * '0' 前导零、1~2 位域宽、l 长度修饰（%ld %lu %lx）。
 * 不支持浮点（软浮点平台，请放大成整数打印）。 */
void uart_printf(const char *fmt, ...);

#endif /* BSP_UART_H */
