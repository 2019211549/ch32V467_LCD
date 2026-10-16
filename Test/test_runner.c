/*********************************************************************
 * test_runner.c - 单元测试模式入口：一行一个用例，直接调用
 *
 * 新增一个测试三步：
 *   1. Test/ 下新建 test_<module>.c，实现 void test_<module>(void)
 *      （整文件用一层 #if PSG_TEST_<MODULE> 包住，模板见 test_sanity.c）
 *   2. Config/test_config.h 加 #define PSG_TEST_<MODULE> 1
 *   3. 下面加一行 TEST_RUN(test_<module>)
 *
 * 模式切换在 Config/psg_config.h 的 PSG_MODE（APP/UNIT_TEST/INTEGRATION），
 * 非单元测试模式下本文件编译为空实现。集成测试以后另建入口文件，
 * 同样由 PSG_MODE 选择，与单元测试互斥。
 *********************************************************************/
#include "psg_config.h"
#include "test_runner.h"

#if PSG_TEST_ENABLE

#include "test_config.h"
#include "test_framework.h"

void test_sanity(void);
void test_bsp_psram(void);
void test_bsp_lcd(void);
void test_lcd_perf(void);
void test_lvgl_hello(void);
void test_task_ui(void);
void test_bsp_touch(void);
void test_t3_swipe(void);
void test_te_explore(void);

/* 跑一个用例并打印结果（失败计数没涨就算通过） */
#define TEST_RUN(fn)                                                  \
    do {                                                              \
        uint32_t _fail0 = test_fail_cnt;                              \
        uart_printf("[RUN ] %s\r\n", #fn);                            \
        fn();                                                         \
        uart_printf("[%s] %s\r\n",                                    \
                (test_fail_cnt == _fail0) ? " OK " : "FAIL", #fn);    \
    } while (0)

int test_runner_run(void)
{
    uart_printf("\r\n===== PSG unit tests =====\r\n");

#if PSG_TEST_SANITY
    TEST_RUN(test_sanity);
#endif
#if PSG_TEST_BSP_PSRAM
    /* 独立用例：只依赖芯片本身（不依赖屏/调度器）；init FAIL 也会
     * 跑完诊断打印（失配形态是降档定位的依据） */
    TEST_RUN(test_bsp_psram);
#endif
#if PSG_TEST_BSP_LCD
    TEST_RUN(test_bsp_lcd);
#endif
#if PSG_TEST_LCD_PERF
    TEST_RUN(test_lcd_perf);
#endif
#if PSG_TEST_LVGL_HELLO
    TEST_RUN(test_lvgl_hello);
#endif
#if PSG_TEST_TASK_UI
    /* 特殊用例：这里只创建 UI+裁判任务，真正判定行在调度器启动 6s 后
     * 由 judge 任务打印（对象是调度器里的行为，runner 等不到它） */
    TEST_RUN(test_task_ui);
#endif
#if PSG_TEST_BSP_TOUCH
    /* 交互用例：坐标打印在调度器里持续输出，判定靠人工看串口 */
    TEST_RUN(test_bsp_touch);
#endif
#if PSG_TEST_T3_SWIPE
    /* 交互用例：3s 自动演示 snap，8s judge 判定行；之后人工手滑验手感 */
    TEST_RUN(test_t3_swipe);
#endif
#if PSG_TEST_TE_EXPLORE
    /* 探索用例：TE 命令打进去、RDDST 状态读回来（无 TE 引脚的替代观察口） */
    TEST_RUN(test_te_explore);
#endif

    uart_printf("===== %lu passed, %lu failed =====\r\n\r\n",
           (unsigned long)test_pass_cnt, (unsigned long)test_fail_cnt);

    return (int)test_fail_cnt;
}

#else /* !PSG_TEST_ENABLE：非单元测试模式，空实现 */

int test_runner_run(void)
{
    return 0;
}

#endif /* PSG_TEST_ENABLE */
