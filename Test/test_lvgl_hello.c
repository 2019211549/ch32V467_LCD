/*********************************************************************
 * test_lvgl_hello.c - LVGL 首例：屏上显示 "Hello LVGL!"（硬件在环）
 *
 * 验证链路：lv_init -> 注册显示屏(flush_cb) -> 建 label ->
 *           lv_refr_now 一次性渲染 -> 分块 flush -> SPI -> 屏幕出字
 *
 * 运行时机：test_runner 在调度器启动前逐个跑用例，本用例不依赖
 * FreeRTOS/tick —— lv_refr_now 是"立即强制渲染"，不走定时器调度
 *
 * 屏上怎么读结果（故意用纯红字，白底）：
 *   红字       = RGB565 全链路正确
 *   蓝字       = 驱动 MADCTL 要加 BGR 位（bsp_lcd.c rot_madctl |0x08）
 *   青/青白底  = IPS 极性反了，INVON 换 INVOFF
 *********************************************************************/
#include "psg_config.h"
#include "test_config.h"

#if PSG_TEST_LVGL_HELLO

#include "test_framework.h"
#include "bsp_lvgl_port.h"
#include "bsp_lcd.h"      /* LCD_ROT_xxx 方向常量 */
#include "lvgl.h"

void test_lvgl_hello(void)
{
    uint32_t blk0;

    /* 1. 移植层：LCD + lv_init + 显示屏注册（画布 15K, RGB565, 部分刷新）
     *    方向在此指定：产品要横屏（320x240）；字若倒着换 LANDSCAPE_270 */
    bsp_lvgl_port_init(LCD_ROT_LANDSCAPE); //进行屏幕的初始化工作//配置回调函数、缓冲区、颜色等

    TEST_ASSERT(lv_display_get_default() != NULL); //获取已经存在的默认显示设备

    /* 2. 业务层：这就是以后写 UI 的全部套路 —— 建控件、设内容、摆位置 */
    lv_obj_t *label = lv_label_create(lv_screen_active());  //第一次创建display时就自动内部创建了一个screen
    TEST_ASSERT(label != NULL);
    lv_label_set_text(label, "Hello LVGL!");  //对控件进行设置
    lv_obj_set_style_text_color(label, lv_color_hex(0xFF0000), 0); /* 纯红=颜色诊断 */
    lv_obj_center(label);                                          /* 屏幕居中 */

    /* 3. 立即渲染一次：LVGL 把无效区域按画布切块，逐块调 flush_cb */
    blk0 = bsp_lvgl_port_flush_cnt();
    lv_refr_now(NULL);

    uart_printf("  flush 分块下发: %lu 块 (15K 画布, 横屏每块最多 24 行)\r\n",
                (unsigned long)(bsp_lvgl_port_flush_cnt() - blk0));

    /* label 建出来且至少下发过 1 块，链路即通 */
    TEST_ASSERT(bsp_lvgl_port_flush_cnt() > blk0);
}

#endif /* PSG_TEST_LVGL_HELLO */
