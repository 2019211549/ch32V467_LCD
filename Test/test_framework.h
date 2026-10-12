/*********************************************************************
 * test_framework.h - 极简单元测试框架（为 MCU 板上运行设计）
 *
 * - 断言失败不中止，只计数并打印位置，跑完所有用例后统一汇总
 * - 浮点注意：本工具链 newlib-nano 关闭了 %f，
 *   打印浮点结果请放大成整数（如 x1000）；
 *   TEST_ASSERT_NEAR 内部只做比较，不打印浮点
 *********************************************************************/
#ifndef TEST_FRAMEWORK_H
#define TEST_FRAMEWORK_H

#include <stdint.h>

/* 断言计数器 */
extern volatile uint32_t test_pass_cnt;
extern volatile uint32_t test_fail_cnt;

void test_fail(const char *file, int line, const char *expr);
void test_fail_int(const char *file, int line, const char *expr,
                   long expect, long actual);

/* 条件为真 -> 通过计数 +1；为假 -> 记录文件/行/表达式 */
#define TEST_ASSERT(cond)                                              \
    do {                                                               \
        if (cond) { test_pass_cnt++; }                                 \
        else { test_fail(__FILE__, __LINE__, #cond); }                 \
    } while (0)

/* 整数相等断言，失败时打印期望值/实际值 */
#define TEST_ASSERT_EQ_INT(expect, actual)                             \
    do {                                                               \
        long _e = (long)(expect), _a = (long)(actual);                 \
        if (_e == _a) { test_pass_cnt++; }                             \
        else { test_fail_int(__FILE__, __LINE__,                       \
                             #expect " == " #actual, _e, _a); }        \
    } while (0)

/* 浮点近似断言：|actual - expect| <= tol */
#define TEST_ASSERT_NEAR(expect, actual, tol)                          \
    do {                                                               \
        double _e = (double)(expect), _a = (double)(actual);           \
        double _d = _a - _e;                                           \
        if ((_d < -(double)(tol)) || (_d > (double)(tol)))             \
            { test_fail(__FILE__, __LINE__, #actual " ~= " #expect); } \
        else { test_pass_cnt++; }                                      \
    } while (0)

#endif /* TEST_FRAMEWORK_H */
