/*********************************************************************
 * bsp_lvgl_port.c - LVGL 9.6 ↔ bsp_lcd 胶水层实现
 *
 * LVGL 内存开销（都在 .bss，改大小改这里/改 lv_conf.h）：
 *   - 32K  lv_malloc 内存池（lv_conf.h 的 LV_MEM_SIZE，控件/样式从中分配）
 *   - 15K  部分刷新画布 s_draw_buf（LVGL 先画这块，再整块发给屏）
 *
 * 坐标系：LVGL 与 bsp_lcd 用同一套"目标方向逻辑坐标"——init 先
 * set_rotation 转屏再量宽高注册（横屏 320x240），两边天然一致。
 *********************************************************************/
#include "bsp_lvgl_port.h"
#include "bsp_lcd.h"
#include "lvgl.h"

/* 部分刷新画布：按最大逻辑宽 320（横屏）x 24 行 x 2 字节 = 15K。
 * LVGL 按"当前 hor_res"自动折算每块行数：竖屏 240 宽时每块 32 行，
 * 横屏 320 宽时每块 24 行 —— 同一块缓冲两种方向都能用。
 * LV_DRAW_BUF_ALIGN=4（lv_conf.h），静态数组按 4 对齐 */
static uint8_t s_draw_buf[320 * 24 * 2] __attribute__((aligned(4)));

/* 教学计数：flush 次数（一次 lv_refr_now 会按画布大小切成 N 块） */
static uint32_t s_flush_cnt;

/*---------------------------------------------------------------------
 * flush_cb —— 移植的全部核心就这一个函数
 * LVGL 在画布里画完 area 区域的一块后回调这里：
 *   area   = 这块像素的逻辑坐标（x1,y1)-(x2,y2)            矩形区域坐标
 *   px_map = 画好的 RGB565 像素流（行优先，与 area 等宽）  画好的内存首地址
 * 驱动是"先设窗口再连续写像素"，正好两行翻译完
 *-------------------------------------------------------------------*/
static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    (void)disp;

    bsp_lcd_set_window((uint16_t)area->x1, (uint16_t)area->y1,
                       (uint16_t)area->x2, (uint16_t)area->y2);
    bsp_lcd_write_pixels((const uint16_t *)px_map,
        (uint32_t)(area->x2 - area->x1 + 1) * (area->y2 - area->y1 + 1));

    s_flush_cnt++; //统计回调次数

    /* 轮询发送，发完即就绪。以后换 SPI DMA：这里只启动传输，
     * lv_display_flush_ready 挪到 DMA 完成中断里调 */
    lv_display_flush_ready(disp);
}

void bsp_lvgl_port_init(uint8_t rotation)
{
    bsp_lcd_init();                    /* GPIO/SPI1/ST7789 初始化（默认竖屏） */
    bsp_lcd_set_rotation(rotation);    /* 转到目标方向，只重发 MADCTL 不清屏 */

    lv_init();                         /* LVGL 内部状态初始化（只需一次） */

    /* 按 LCD 当前逻辑分辨率建显示屏（横屏 320x240 / 竖屏 240x320） */  //lvgl可以设置几个
    lv_display_t *disp = lv_display_create(bsp_lcd_width(), bsp_lcd_height());
    if (disp == NULL) {

        return;   /* 内存池不够会走到这：查 lv_conf.h 的 LV_MEM_SIZE。
                     接口不返回错误码，由调用方断言 lv_display_get_default() 兜底 */
    }

    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565); /* 与驱动 COLMOD 0x55 一致 */
    lv_display_set_flush_cb(disp, flush_cb);  //当你绘图完成，需要更新 LCD 时，就调用我提供的 flush_cb 函数。”
    lv_display_set_buffers(disp, s_draw_buf, NULL, sizeof(s_draw_buf),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);  //  告诉 LVGL 使用哪一块内存绘图、这块内存有多大，以及按照什么模式进行刷新。 //这里可以实现双缓冲

}

uint32_t bsp_lvgl_port_flush_cnt(void)
{
    return s_flush_cnt;
}
