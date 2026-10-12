/*********************************************************************
 * bsp_uart.c - 调试串口打印实现
 *
 * 之前的链路：printf(newlib-nano 库函数) -> _write 桩 -> USART
 * 现在的链路：uart_printf(自写格式化器) -> uart_send -> USART
 * 不再依赖库的 printf/vsnprintf：无堆、无 libc 内部锁、代码更小。
 *
 * 注意：查询阻塞发送，多任务并发调用时输出可能交叉；
 *      需要独占输出时在调用侧用互斥量包一层（将来加 log 锁即可）。
 *********************************************************************/
#include <stdarg.h>
#include <stdint.h>
#include "debug.h"
#include "bsp_uart.h"

/* 当前调试口对应的 USART 外设 */
static USART_TypeDef *dbg_usart(void)
{
#if (DEBUG == DEBUG_USART1)
    return USART1;
#elif (DEBUG == DEBUG_USART2)
    return USART2;
#else
    return USART3;
#endif
}

void bsp_uart_debug_init(uint32_t baudrate)
{
    /* GPIO/USART 初始化复用 vendor 实现，避免维护两份寄存器配置 */
    USART_Printf_Init(baudrate);
}

void uart_send(const char *buf, uint32_t len)
{
    USART_TypeDef *u = dbg_usart();
    uint32_t i;

    for (i = 0; i < len; i++)
    {
        while (USART_GetFlagStatus(u, USART_FLAG_TC) == RESET)
        {
        }
        USART_SendData(u, (uint16_t)(uint8_t)buf[i]);
    }
}

void uart_puts(const char *s)
{
    while (*s != '\0')
    {
        uart_send(s, 1);
        s++;
    }
}

/*********************************************************************
 *                      ↓↓↓ 内部格式化实现 ↓↓↓
 *********************************************************************/
static void pr_char(char c)
{
    uart_send(&c, 1);
}

/* 按 base 进制打印无符号数，可带符号/域宽/前导零 */
static void pr_num(unsigned long v, unsigned int base, int upper,
                   int neg, int width, char pad)
{
    char digits[12];    /* 32 位十进制最多 10 位，留余量 */
    const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int nd = 0;
    int total;

    if (v == 0)
    {
        digits[nd++] = '0';
    }
    while (v != 0)
    {
        digits[nd++] = dig[v % base];
        v /= base;
    }
    total = nd + (neg ? 1 : 0);

    if (pad == '0')     /* 前导零：符号在最前，如 %08d 的 -42 -> -0000042 */
    {
        if (neg)
        {
            pr_char('-');
        }
        while (width > total)
        {
            pr_char('0');
            width--;
        }
    }
    else                /* 空格右对齐 */
    {
        while (width > total)
        {
            pr_char(' ');
            width--;
        }
        if (neg)
        {
            pr_char('-');
        }
    }

    while (nd > 0)      /* digits 是逆序的，倒着发 */
    {
        pr_char(digits[--nd]);
    }
}

void uart_printf(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    while (*fmt != '\0')
    {
        char pad = ' ';
        int width = 0;
        int lng = 0;

        if (*fmt != '%')
        {
            pr_char(*fmt++);
            continue;
        }
        fmt++;                          /* 跳过 '%' */

        if (*fmt == '0')                /* '0' 前导零标志 */
        {
            pad = '0';
            fmt++;
        }
        while ((*fmt >= '0') && (*fmt <= '9'))   /* 域宽 */
        {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }
        while (*fmt == 'l')             /* l 长度修饰（可重复出现） */
        {
            lng = 1;
            fmt++;
        }

        switch (*fmt)
        {
            case 'd':
            case 'i':
            {
                long v = lng ? va_arg(ap, long) : (long)va_arg(ap, int);
                if (v < 0)
                {
                    pr_num((unsigned long)(-v), 10, 0, 1, width, pad);
                }
                else
                {
                    pr_num((unsigned long)v, 10, 0, 0, width, pad);
                }
                break;
            }
            case 'u':
            {
                unsigned long v = lng ? va_arg(ap, unsigned long)
                                      : (unsigned long)va_arg(ap, unsigned int);
                pr_num(v, 10, 0, 0, width, pad);
                break;
            }
            case 'x':
            case 'X':
            {
                unsigned long v = lng ? va_arg(ap, unsigned long)
                                      : (unsigned long)va_arg(ap, unsigned int);
                pr_num(v, 16, (*fmt == 'X'), 0, width, pad);
                break;
            }
            case 'p':
            {
                uart_puts("0x");
                pr_num((unsigned long)(uintptr_t)va_arg(ap, void *), 16, 0, 0, 0, ' ');
                break;
            }
            case 'c':
            {
                pr_char((char)va_arg(ap, int));
                break;
            }
            case 's':
            {
                const char *s = va_arg(ap, const char *);
                int len = 0;

                if (s == NULL)
                {
                    s = "(null)";
                }
                while (s[len] != '\0')
                {
                    len++;
                }
                while (width > len)         /* 域宽右对齐（空格补齐），与数字一致 */
                {
                    pr_char(' ');
                    width--;
                }
                while (*s != '\0')
                {
                    pr_char(*s++);
                }
                break;
            }
            case '%':
            {
                pr_char('%');
                break;
            }
            case '\0':                  /* 格式串意外终止 */
            {
                va_end(ap);
                return;
            }
            default:                    /* 未知占位符：'%' 原样输出 */
            {
                pr_char('%');
                pr_char(*fmt);
                break;
            }
        }
        fmt++;
    }
    va_end(ap);
}
