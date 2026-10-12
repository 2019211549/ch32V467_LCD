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

/* 教学计数：从上电起 flush_cb 被调用的次数（= 已下发多少块画布） */
uint32_t bsp_lvgl_port_flush_cnt(void);

#endif /* BSP_LVGL_PORT_H */
