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

/* sanity：框架自检，验证断言宏和 runner 本身能正常工作
 * 2026-10-16 起置 0：列优先学习用例独占固件（其余宏同批清零） */
#define PSG_TEST_SANITY         0

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
#define PSG_TEST_T3_SWIPE       0   /* 2026-10-16 起走竖板追扫生产路径：
                                       bsp_lvgl_port_init_slab（PSRAM staging +
                                       GSCAN 回绕对齐 + 16 列板 DMA 流水，板证
                                       零撕裂）；PSRAM/GSCAN 不具备自动回退
                                       24 行部分刷新（judge 行 slab=0 标出）。
                                       同日置 0：列优先学习用例独占固件 */

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
 * （不动 Link.ld/LVGL）。init FAIL 也会跑完诊断打印（失配形态定位）。
 * 2026-10-16 置 0：列优先学习用例独占固件（PSRAM 300M 板证已收官） */
#define PSG_TEST_BSP_PSRAM      0

/* GSCAN(0x45) 扫描行验证（硬件在环，需 SDO 接好）：读通路自检 + 应答门 +
 * 锯齿/回绕周期 T_scan + 频率梯（四档读速找最快可信档）+ 斜率自洽。
 * 纯测量不做同步（回绕点=帧头?下阶段翻色标定）。跑在调度器前，
 * 与 T4 兼容可同开；只读命令零残留。
 * 2026-10-16 置 0：列优先学习用例独占固件（GSCAN 标定已收官） */
#define PSG_TEST_LCD_GSCAN      0

/* 翻色标定（硬件在环，需人眼看屏/慢动作拍摄）：阶段A 横带基线（已知斜
 * 撕裂线，对照）；阶段B 16列竖板同向追扫 δ0=1650µs（写落后扫描一整板
 * 785µs+Δ+余量，预期零撕裂）；阶段C δ0 阶梯（斜纹消失的最小 δ0* ≈
 * 785+Δ → 反解回绕-帧头偏移 Δ）。竖板 = 乒乓双板 SRAM + 每板窗口 +
 * 异步 DMA（生产目标形态）。观察结果靠人发回，断言只管机械量。跑在
 * 调度器前，t3 之前。
 * 2026-10-16 置 0：标定收官（δ0*=1950，生产 δ0=2100），记录见
 * Docs/LCD撕裂根治_扫描同步竖板追扫_原理与标定记录.md */
#define PSG_TEST_LCD_FLIPCAL    0

/* 列优先 DMA 演示+翻色（硬件在环，只依赖屏+SDO；LCD 深入学习 1/2 课，
 * 2026-10-16 建课，10-19 板证 MADCTL=0x40 定案）：横屏不变，每板 =
 * 转置窗口 + 列连续缓冲 + 单笔 3840 半字异步 DMA。演示段：1s/板慢扫
 * 看写前沿沿 u 推进（与扫描同向）。翻色第二轮（10-19）：① δ0 细钉梯
 * {1120,1180,1240}µs 钉 Δ（一轮板证 1100 有线/1300 无线）；② 节奏
 * 裁剪梯（δ0=0）pace {T/20+8, T/20, 背靠背}——线右移不消失=扫描快于
 * DMA 极限、同帧不可达；线消失=δ0=0 同帧翻色成立。每档 4s。
 * 独立：不碰 PSRAM/LVGL */
#define PSG_TEST_LCD_COLMAJOR   0

/* T5 列优先直通滑动（硬件在环，需 SDO 接好）：列优先 GRAM 底座 +
 * LVGL 竖屏 240x320 声明 + 双画布 16 列条带直接 DMA（零转置/零 PSRAM）
 * + 每周期首条 GSCAN 回绕对齐（δ0=0 同帧翻色）。UI=一个 screen+3 容器
 * 竖排 snap 翻页（竖屏坐标，屏上观感左右滑）。3s 自动演示，8s judge
 * 判定行（含 cm 帧周期/ sync_miss），之后手滑与 T3 对比手感/A-B 结论。
 * 与 T3/T4/BSP_TOUCH 互斥
 * 2026-10-19 置 0：T6 横屏课独占固件（T5 板证顺滑已收官） */
#define PSG_TEST_T5_COLMAJOR_SWIPE 0

/* T6 列优先横屏直通滑动（硬件在环，需 SDO 接好）：T5 物理底座 + 横屏
 * 320x240 声明兼得 —— LVGL 内核补丁 LV_USE_COLMAJOR_RENDER 让 PARTIAL
 * 脏区按列切 16 宽 x 全高竖条，flush_cb 转置(行主序→列主序) + 交换参数
 * 窗口 + 乒乓双缓冲异步 DMA（发板 k 同时渲染板 k+1，转置完立即
 * flush_ready 释放单画布），周期首条排干在飞 DMA 后 GSCAN 回绕对齐
 * （δ0=0 同帧翻色，物理同 T5）。UI=横排 3 页 + 正向文字 + F 字标 +
 * 四角色标 + OK 按钮（对照 T5 的横躺+镜像文本）。3s 自动演示，8s judge
 * （含 cmlm 帧周期/sync_miss/转置 max/块间隙 avg-max，间隙 avg<~42µs
 * = 渲染+转置藏进 819µs DMA 影子）。与 T3/T4/T5/BSP_TOUCH 互斥 */
#define PSG_TEST_T6_COLMAJOR_LM 1

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
#define PSG_TEST_LCD_COLMAJOR   0
#define PSG_TEST_T5_COLMAJOR_SWIPE 0
#define PSG_TEST_T6_COLMAJOR_LM 0

#endif
#endif /* TEST_CONFIG_H */
