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
 * （两个任务各跑一套 LVGL 抢渲染），与 PSG_TEST_BSP_TOUCH 也互斥，
 * 与 T4 也互斥（同款 UI 任务）—— A/B 对比时 T3/T4 轮流置 1 */
#define PSG_TEST_T3_SWIPE       1   /* 回 24 行 SRAM 条带路径（T4 PSRAM 整帧
                                       帧周期 36ms 判定太慢，暂弃，分支保留） */

/* T4 整帧 PSRAM 滑动（硬件在环）：与 T3 同一套三页横滑 UI，画布换成
 * PSRAM 两块 150K 整帧缓冲（DIRECT 全局渲染 + 脏区切条带逐条异步 DMA，
 * 一帧 3 条带适配单笔 32768 半字上限）。judge@8s 量帧周期 avg/max ——
 * 和 T3 的手感/flush 增速对比就是 A/B 结论。PSRAM 不在线自动回退部分
 * 刷新（judge 判 ff=0 FAIL，不黑屏）。互斥同 T3。
 * 2026-10-16 板证：机制成立（18帧/54条带 DMA 全对），但 avg 36ms/28fps
 * 比 T3 慢（refr_sync_areas 拷贝 + 每帧整帧传输），后续撕裂同步走 T3
 * 的 24 行 SRAM 条带路径，本用例留作对照 */
#define PSG_TEST_T4_FULLFRAME   0

/* TE 命令探索（硬件在环）：本模组没焊 TE 引脚，但 TEON/TEOFF 的生效
 * 与否可从 RDDST(0x09) 状态字读回验证——本用例打全套 TE 命令并逐字节
 * 对比状态变化。纯驱动层，不需要调度器。看完置 0（探索性用例，无常驻价值） */
#define PSG_TEST_TE_EXPLORE     0

/* 片上 PSRAM 独立单元测试（硬件在环，只依赖芯片本身）：EVT 初始化
 * （EVT 例程同款：SYSCLK 300M + 300M 档 + trc=0x14 + die MR 复位）+
 * 三区间图案扫描 + 地址线别名自检 + 带宽粗测。与其它用例零耦合
 * （不动 Link.ld/LVGL）。init FAIL 也会跑完诊断打印（失配形态定位） */
#define PSG_TEST_BSP_PSRAM      1

/* GSCAN(0x45) 扫描行验证（硬件在环，需 SDO 接好）：读通路自检 + 应答门 +
 * 锯齿/回绕周期 T_scan + 频率梯（四档读速找最快可信档）+ 斜率自洽。
 * 纯测量不做同步（回绕点=帧头?下阶段翻色标定）。跑在调度器前，
 * 与 T4 兼容可同开；只读命令零残留 */
#define PSG_TEST_LCD_GSCAN      1

/* 翻色标定（硬件在环，需人眼看屏/慢动作拍摄）：阶段A 横带基线（已知斜
 * 撕裂线，对照）；阶段B 16列竖板同向追扫 δ0=1650µs（写落后扫描一整板
 * 785µs+Δ+余量，预期零撕裂）；阶段C δ0 阶梯（斜纹消失的最小 δ0* ≈
 * 785+Δ → 反解回绕-帧头偏移 Δ）。竖板 = 乒乓双板 SRAM + 每板窗口 +
 * 异步 DMA（生产目标形态）。观察结果靠人发回，断言只管机械量。跑在
 * 调度器前，t3 之前 */
#define PSG_TEST_LCD_FLIPCAL    1

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
#define PSG_TEST_T4_FULLFRAME   0
#define PSG_TEST_TE_EXPLORE     0
#define PSG_TEST_BSP_PSRAM      0
#define PSG_TEST_LCD_GSCAN      0
#define PSG_TEST_LCD_FLIPCAL    0

#endif
#endif /* TEST_CONFIG_H */
