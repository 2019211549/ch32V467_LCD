/*********************************************************************
 * psg_config.h - PSG 项目总配置（唯一的"开关面板"）
 *
 * 约定：
 *   1. 功能开关命名   PSG_FEATURE_<MODULE>
 *   2. 调试开关命名   PSG_DEBUG_<NAME>
 *   3. 测试开关命名   PSG_TEST_<NAME>（模块级细分开关在 test_config.h）
 *   4. 模块没引入之前不要提前定义开关，引入模块时再加，
 *      避免一堆指向不存在代码的死开关
 *********************************************************************/
#ifndef PSG_CONFIG_H
#define PSG_CONFIG_H

#include "bsp_uart.h"   /* 日志出口：自定义 uart_printf（Bsp/bsp_uart.c） */

/* ==================== 调试 ==================== */
#define PSG_DEBUG_LOG           1   /* 板上调试打印(USART1) 总开关 */

#if PSG_DEBUG_LOG
#define PSG_LOG(fmt, ...)   uart_printf(fmt, ##__VA_ARGS__)
#else
#define PSG_LOG(fmt, ...)   do { } while (0)
#endif

/* ==================== 固件运行模式（要跑哪个模式只改这一行） ====================
 * APP          正式业务固件：不编译、不运行任何测试
 * UNIT_TEST    单元测试模式：调度器启动前逐个跑 Test/ 用例（当前）
 * INTEGRATION  集成测试模式：预留，整机联调用例以后另建入口
 *
 * 模式互斥：切到某个模式，其余模式的代码全部编译为空（0 flash/RAM） */
#define PSG_MODE_APP            0
#define PSG_MODE_UNIT_TEST      1
#define PSG_MODE_INTEGRATION    2

#define PSG_MODE                PSG_MODE_UNIT_TEST /* 检查点 B 已收编为用例 test_task_ui（调度器联测），APP 模式留给正式业务 */

/* 测试框架总使能 = 是否处于单元测试模式（不要直接改这个） */
#define PSG_TEST_ENABLE         (PSG_MODE == PSG_MODE_UNIT_TEST)

#endif /* PSG_CONFIG_H */
