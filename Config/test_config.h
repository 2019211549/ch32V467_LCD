/*********************************************************************
 * test_config.h - 单元测试的模块级开关（只在 PSG_TEST_ENABLE=1 时生效）
 *
 * 命名：PSG_TEST_<MODULE>
 *   纯逻辑测试（Algorithm/，无硬件依赖）：随时可开
 *   硬件在环测试（Bsp/，需接好硬件再开）：  默认置 0
 *********************************************************************/
#ifndef TEST_CONFIG_H
#define TEST_CONFIG_H

#include "psg_config.h"

#if PSG_TEST_ENABLE

/* sanity：框架自检，验证断言宏和 runner 本身能正常工作 */
#define PSG_TEST_SANITY         1

/* LCD 点屏测试（硬件在环）：接好 ST7789P3 屏后置 1，屏未接时置 0。
 * 注意会拖慢开机 ~6s（纯色轮播），点亮验证完建议置回 0。
 * 2026-09-24 点屏已验证通过，性能测试期间置 0（其末尾 while(1) 会挡住后续用例） */
#define PSG_TEST_BSP_LCD        0

/* LCD 性能基线/阶梯测试（阶段0~10 已收官，记录见 Docs/lcd_perf_test_record.md
 * 与 lcd_opt_stage10.md）。平时置 0，要跑性能回归再开 */
#define PSG_TEST_LCD_PERF       0

/* LCD 接线自检（step0）：5 根信号线打静态电平，配合万用表在 FPC 侧
 * 逐根核对（会停在 while(1) 等人量）。接线核对通过后置回 0 */
#define PSG_TEST_LCD_WIRING     0

/* LVGL 首例（硬件在环）：屏上显示 "Hello LVGL!"，打通 LVGL→flush_cb→SPI 链路
 * （部分刷新 + RGB565）。观察口诀：红字=全对  蓝字=驱动MADCTL要加BGR  青字=INVON极性反 */
#define PSG_TEST_LVGL_HELLO     0

/* 检查点 B（硬件在环）：task_ui 异步 DMA + 信号量 + 调度器联测。
 * 注意本用例的判定行在调度器启动 6s 后才打印（见 test_task_ui.c 文件头）。
 * T3 期间置 0：t3_ui 会另建一套页面，两个 UI 任务抢渲染 */
#define PSG_TEST_TASK_UI        0

/* 触摸 T1（硬件在环）：FT6336 坐标串口流式打印（I3C legacy 传输，2026-10-13
 * 板证通过）。T2 起触摸由 LVGL indev 路径接管（33ms 真链路轮询
 * + touch 日志），与本用例的 100ms 轮询任务会并发打 I3C —— 平时置 0，
 * 要单独回归触摸驱动时再开（开着会和 indev 抢总线，err 会涨） */
#define PSG_TEST_BSP_TOUCH      0

/* T3 横滑切页（硬件在环）：3 整屏彩色页 + scroll snap 居中吸附，验证
 * 跟手拖动 / 松手回弹 / 惯性翻页（iPhone 式分页）。3s 自动演示滑一页
 * （无手指也能看 snap），8s judge 判定行。与 PSG_TEST_TASK_UI 互斥
 * （两个任务各跑一套 LVGL 抢渲染），与 PSG_TEST_BSP_TOUCH 也互斥 */
#define PSG_TEST_T3_SWIPE       1

/* TE 命令探索（硬件在环）：本模组没焊 TE 引脚，但 TEON/TEOFF 的生效
 * 与否可从 RDDST(0x09) 状态字读回验证——本用例打全套 TE 命令并逐字节
 * 对比状态变化。纯驱动层，不需要调度器。看完置 0（探索性用例，无常驻价值） */
#define PSG_TEST_TE_EXPLORE     1

/* 后续模块的开关在这里加，例如：
 *   #define PSG_TEST_ALG_FILTER  1
 */

#else /* 非单元测试模式（APP/集成）：所有用例开关强制为 0，
       * 这样各测试文件只需一层 #if PSG_TEST_<MODULE> 守卫 */

#define PSG_TEST_SANITY         0
#define PSG_TEST_BSP_LCD        0
#define PSG_TEST_LCD_WIRING     0
#define PSG_TEST_LCD_PERF       0
#define PSG_TEST_LVGL_HELLO     0
#define PSG_TEST_TASK_UI        0
#define PSG_TEST_BSP_TOUCH      0
#define PSG_TEST_T3_SWIPE       0
#define PSG_TEST_TE_EXPLORE     0

#endif
#endif /* TEST_CONFIG_H */
