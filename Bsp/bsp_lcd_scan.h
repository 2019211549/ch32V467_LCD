/*********************************************************************
 * bsp_lcd_scan.h - GSCAN 扫描同步 + 16 列竖板追扫写板器（生产路径）
 *
 * 解决什么：24 行横带写入与面板扫描轴垂直，数学上必有斜向撕裂
 * （v=0.764u，板证）；本模块把整帧更新改为与扫描同轴的竖板写入，
 * 用 GSCAN(0x45) 回绕对齐 + 标定参数配速，板证零撕裂（2026-10-16，
 * 标定实验见 Test/test_lcd_flipcal.c）。
 *
 * 标定参数（板证）：扫描=横屏左→右 52.33µs/列；回绕早于可见帧头
 * Δ≈1.1ms；"写一块板(819µs)整段不得与扫描在该板内重叠"
 *   → 起写 δ0 = 2100µs（零撕裂阈值 1950 + 余量）
 *   → 板配速 pace = T_scan/20 + 8 ≈ 845µs（略慢于扫描 837µs）
 *   → 整屏颜色在扫描趟边界统一翻转（帧量化，等效 vsync 页翻转）
 *
 * 数据通路：调用方（bsp_lvgl_port 竖板模式）把 LVGL 渲染好的横带逐块
 * band_put 进 PSRAM 整帧 staging；一帧渲染完调 frame_send 追扫上屏：
 * 等回绕 + δ0 → 20 板流水（DMA 写板 k 时 CPU 从 staging gather 板 k+1，
 * gather 藏在 DMA 的 819µs 里）。staging 兼任"持久帧缓冲"——脏区外的
 * 旧内容自动保序，无脏区就不调 frame_send（静态界面零成本）。
 *
 * 依赖：只依赖 bsp_lcd / bsp_psram（不依赖 RTOS/LVGL，头文件不暴露
 * 任何 lvgl 类型）。使用前置条件：bsp_lcd_init + set_rotation(横屏
 * 320x240) 已完成（GSCAN 读取与坐标系都依赖它）。
 *********************************************************************/
#ifndef BSP_LCD_SCAN_H
#define BSP_LCD_SCAN_H

#include <stdint.h>

/* 初始化 + 自检（幂等，重复调用直接返回 0）。
 * 前置：bsp_lcd_init + set_rotation(横屏) 已跑；PSRAM 幂等初始化在内部做。
 * 返回 0 = 就绪；1 = PSRAM 自检不过（staging 不可用）；
 * 2 = 当前不是横屏 320x240（标定坐标系限制）；3 = GSCAN 无回绕（SDO
 * 未接/屏不响应）；4 = T_scan 超出 25~125Hz 常识窗 */
int bsp_lcd_scan_init(void);

/* 是否已成功初始化（1=是） */
uint8_t bsp_lcd_scan_ready(void);

/* 把一块渲染好的横带拷进 PSRAM staging（RGB565）。
 * px 按带宽紧密排布（LVGL 画布语义：行距 = x1-x0+1，不是屏宽）。
 * 必须在 init 成功后调用；未就绪返回 -1 */
int bsp_lcd_scan_band_put(uint16_t x0, uint16_t y0,
                          uint16_t x1, uint16_t y1,
                          const uint16_t *px);

/* 追扫整帧上屏（阻塞：回绕等待 0~16.7ms + 追扫 ~20ms）。
 * staging 内容一次性按 20 板流水写出；帧末调用一次。
 * 返回本帧追扫耗时 µs；<0 = 失败（-1 回绕超时 -2 DMA 超时 -3 未就绪），
 * 失败不抛异常，调用方下一帧重试即可 */
int32_t bsp_lcd_scan_frame_send(void);

/* 实测扫描周期 µs（init 后有效；0 = 未初始化） */
uint32_t bsp_lcd_scan_tscan_us(void);

/* 诊断：累计帧数 / 平均·最大追扫耗时 µs / 每帧平均 gather 耗时 µs /
 * 超时次数（回绕+DMA）。参数可传 0 跳过 */
void bsp_lcd_scan_stats(uint32_t *frames, uint32_t *avg_us, uint32_t *max_us,
                        uint32_t *gather_avg_us, uint32_t *timeouts);

#endif /* BSP_LCD_SCAN_H */
