/*********************************************************************
 * test_runner.h - 单元测试调度入口
 *
 * 用法：在 main() 里、vTaskStartScheduler() 之前调用 test_runner_run()。
 * 返回失败的断言数（0 = 全部通过）。
 * PSG_TEST_ENABLE=0 时提供空实现，调用处无需条件编译包裹。
 *********************************************************************/
#ifndef TEST_RUNNER_H
#define TEST_RUNNER_H

int test_runner_run(void);

#endif /* TEST_RUNNER_H */
