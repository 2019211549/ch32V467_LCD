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

/* 竖板追扫模式（生产路径，代替 bsp_lvgl_port_init 使用）：LCD/lv_init 同
 * 一套，差别是 flush 链路换为 "24 行画布横带 → PSRAM 整帧 staging → 帧末
 * GSCAN 回绕对齐 + 16 列竖板 gather/DMA 流水追扫"（标定参数与板证零撕裂
 * 记录见 bsp_lcd_scan.c 文件头；2026-10-16 收官）。整屏更新帧量化（等效
 * vsync 页翻转，~30fps 上限），无脏区不追扫。不需要 async_enable（调了
 * 无害）。返回 0 = 竖板模式生效；1 = PSRAM/GSCAN 不具备，已回退部分刷新
 * （显示照常，撕裂回到 24 行横带形态）。横屏 320x240 专用（标定坐标系）。
 * 幂等 */
uint8_t bsp_lvgl_port_init_slab(uint8_t rotation);

/* 当前是否竖板追扫模式（1=是；回退部分刷新后为 0） */
uint8_t bsp_lvgl_port_slab_active(void);

/* 列优先直通模式（T5，代替 bsp_lvgl_port_init 使用，无参数——方向固定）：
 * display 按玻璃原生竖屏 240x320 声明 + MADCTL=0x40（板证列优先地址序，
 * CASET=屏竖直 v / RASET=屏水平 u 扫描轴），LVGL 部分刷新条带（240x16）
 * 物理上就是 16 列 x 全高竖板 —— flush_cb 原样窗口 + 一笔异步 DMA，
 * 零转置/零拷贝/零 PSRAM（画布即 DMA 源）。撕裂同步：每刷新周期首条
 * flush 前 GSCAN 回绕对齐（δ0=0）+ 条带背靠背（板证 20 板 ≈16.6ms < 1
 * 帧 = 同帧干净翻色）→ 等效 vsync 页翻转且无整帧量化等待（~60fps 上限，
 * 对比 T3 追扫的 2 帧周期 ~30fps）。异步三件套在 init 内一步到位（之后
 * 调 async_enable 幂等无害）。需 SDO 在线（GSCAN 读），不在线则放弃
 * 同步照发（可能撕裂，sync_miss 计数标出）。返回 0 = 生效；1 = 本次
 * 上电已按其它模式初始化过（维持原模式）。幂等 */
uint8_t bsp_lvgl_port_init_colmajor(void);

/* 当前是否列优先直通模式（1=是） */
uint8_t bsp_lvgl_port_cm_active(void);

/* 列优先模式刷新统计（参数可传 0 跳过）：frames=完成的刷新周期数，
 * avg/max_period_us=相邻回绕对齐的间隔（= 实际帧周期，60fps 应 ~16.7ms），
 * sync_miss=回绕等待超时数（SDO 不在线/读异常） */
void bsp_lvgl_port_cm_stats(uint32_t *frames, uint32_t *avg_period_us,
                            uint32_t *max_period_us, uint32_t *sync_miss);

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

/* 触摸 indev 坐标轴切换（T5 列优先模式配套）：en=1 时 read_cb 上报
 * 竖屏坐标（LVGL-x=屏v=239-gx、LVGL-y=屏u=gy，由板证横屏映射②换轴）；
 * 默认 0=横屏坐标 (u,v)。touch_init 前后任意时刻可调 */
void bsp_lvgl_port_touch_set_portrait(uint8_t en);

#endif /* BSP_LVGL_PORT_H */
