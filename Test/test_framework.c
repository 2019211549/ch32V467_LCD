/*********************************************************************
 * test_framework.c - 断言实现（PSG_TEST_ENABLE=0 时整个文件为空）
 *********************************************************************/
#include "psg_config.h"
#if PSG_TEST_ENABLE

#include "test_framework.h"

volatile uint32_t test_pass_cnt = 0;
volatile uint32_t test_fail_cnt = 0;

void test_fail(const char *file, int line, const char *expr)
{
    test_fail_cnt++;
    uart_printf("  ASSERT FAILED: %s:%d: %s\r\n", file, line, expr);
}

void test_fail_int(const char *file, int line, const char *expr,
                   long expect, long actual)
{
    test_fail_cnt++;
    uart_printf("  ASSERT FAILED: %s:%d: %s (expect=%ld actual=%ld)\r\n",
           file, line, expr, expect, actual);
}

#endif /* PSG_TEST_ENABLE */
