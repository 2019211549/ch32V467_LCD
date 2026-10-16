/*********************************************************************
 * bsp_lvgl_port.h - LVGL 9.6 ↔ bsp_lcd 胶水层
 *
 * 职责：把 bsp_lcd（ST7789P3 SPI 屏）注册成一块 LVGL 显示屏
 *   - RGB565 部分刷新（PARTIAL）：LVGL 在 15K 画布里画一小块，
 *     flush_cb 翻译成 bsp_lcd_set_window + bsp_lcd_write_pixels 下发
 *   - 本文件不暴露任何 lvgl 类型（不 include lvgl.h），
 *     上层要用 LVGL API 自己 include "lvgl.h"
 *
 * tick 说明：一次性渲染（lv_refr_now）不需要 tick；
 * 以后 App/task_ui 常驻跑 lv_timer_handler 时再补 lv_tick_set_cb
 *********************************************************************/
#ifndef BSP_LVGL_PORT_H
#define BSP_LVGL_PORT_H

#include <stdint.h>

/* 初始化 LCD + LVGL 并注册显示屏（失败时停在内部断言打印）。
 * rotation 传 bsp_lcd.h 的 LCD_ROT_xxx（本头文件不引 bsp_lcd.h）：
 * 先转屏再量宽高，LVGL 显示屏直接按目标方向注册——横屏 320x240，
 * 竖屏 240x320，控件坐标天然就是目标方向的逻辑坐标。
 * 调用一次即可；之后再画控件、lv_refr_now / lv_timer_handler */
void bsp_lvgl_port_init(uint8_t rotation);

/* 开启异步刷新档（检查点 B）：静态二值信号量 + DMA 完成中断回调 +
 * LVGL flush_wait_cb。必须在 bsp_lvgl_port_init 之后、开始渲染之前调。
 * 不调用则保持同步档（检查点 A 语义）；重复调用无副作用（幂等） */
void bsp_lvgl_port_async_enable(void);

/* 整帧 PSRAM 模式（T4 A/B 实验）：代替 bsp_lvgl_port_init 使用 —— LCD/
 * lv_init 同一套，差别是画布换成 PSRAM 里两块整帧缓冲（RENDER_MODE_DIRECT
 * 全局渲染，flush_cb 把脏区按 DMA 单笔上限切条带、逐条异步下发，全程
 * 不退化同步）。之后照常调 bsp_lvgl_port_async_enable + lv_tick_set_cb。
 * 返回 0 = 整帧模式生效；1 = PSRAM 自检不过已回退部分刷新（或本次上电
 * 已按部分刷新初始化过）—— 回退时显示照常工作，只是对比没做成。幂等 */
uint8_t bsp_lvgl_port_init_fullframe(uint8_t rotation);

/* 当前是否整帧 DIRECT 模式（1=是；回退部分刷新后为 0） */
uint8_t bsp_lvgl_port_fullframe_active(void);

/* 整帧模式累计下发条带数（诊断：除以 flush 数 = 每帧条带数，320 宽
 * 全屏一帧应为 3 条：102+102+36 行） */
uint32_t bsp_lvgl_port_strip_cnt(void);

/* 教学计数：flush_cb 被调用的次数。部分刷新=已下发多少块画布；
 * 整帧 DIRECT=已完成多少帧（一次 flush_cb 一帧） */
uint32_t bsp_lvgl_port_flush_cnt(void);

/* 触摸 indev 初始化（T2）：bsp_touch + lv_indev(POINTER) + read_cb。
 * 必须在 bsp_lvgl_port_init 之后调（建默认屏在前）。幂等。
 * 返回 0=成功；1=触摸不在线（不挡显示，返回错误码由调用方决定怎么处理）；
 * 2=没先 init 显示；3=LVGL 内存池不够 */
uint8_t bsp_lvgl_port_touch_init(void);

/* 玻璃坐标 -> 横屏逻辑坐标（纯函数，测试/日志用）：
 * 映射②（板证 2026-10-13）：u=玻璃y, v=239-玻璃x，越界钳位。
 * 实现见 bsp_lvgl_port.c 注释（四角实测数据） */
void bsp_lvgl_port_touch_map(uint16_t gx, uint16_t gy, uint16_t *u, uint16_t *v);

/* read_cb 最近一次读数（原始+映射+状态+累计错误数），诊断打印用。
 * 参数可传 0 跳过 */
void bsp_lvgl_port_touch_dbg(uint16_t *gx, uint16_t *gy, uint16_t *u,
                             uint16_t *v, uint8_t *pressed, uint32_t *err);

#endif /* BSP_LVGL_PORT_H */
