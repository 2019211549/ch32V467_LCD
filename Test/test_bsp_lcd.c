/*********************************************************************
 * test_bsp_lcd.c - 点屏测试（硬件在环）
 *
 * 读回诊断（需要模组 SDO pin7 -> PA6 连线）：
 *   RDDID/RDDST 全 0x00 或全 0xFF -> SPI 回读不通：查 SDO/VCC/接线
 *   RDDST 第 3 字节 bit7=1 -> 面板还在睡眠：SLPOUT 没生效，查 DC/CS 接线
 *   读回有合理数据但屏黑 -> SPI 通，问题在 init 序列（找供应商要 init 表）
 *
 * 预期现象（需要人眼确认）：
 *   1. 屏幕依次刷 8 种纯色（红绿蓝白青品红黄黑），每种停留约 0.6s
 *   2. 最后显示 8 根竖向色条并保持
 *
 * 本测试在调度器启动前运行，用的是软件延时，不依赖 FreeRTOS。
 *********************************************************************/
#include "psg_config.h"
#include "test_config.h"

#if PSG_TEST_BSP_LCD

#include "test_framework.h"
#include "bsp_lcd.h"

/* 本轮测试的屏幕方向（改这一行即可切换横竖屏测试）：
 * LCD_ROT_PORTRAIT / LCD_ROT_LANDSCAPE / LCD_ROT_PORTRAIT_180 / LCD_ROT_LANDSCAPE_270 */
#define LCD_TEST_ROT      LCD_ROT_LANDSCAPE

/* 测试用粗略毫秒延时（调度器未启动） */
static void lcd_test_delay_ms(uint32_t ms)
{
    volatile uint32_t n = ms * 60000UL;

    while (n--)
    {
    }
}

void test_bsp_lcd(void)
{
    static const uint16_t colors[] = {
        LCD_RED, LCD_GREEN, LCD_BLUE,  LCD_WHITE,
        LCD_CYAN, LCD_MAGENTA, LCD_YELLOW, LCD_BLACK,
    };
    uint8_t id[4] = {0, 0, 0, 0};
    uint16_t w, h;
    int bad;
    uint32_t i;

    (void)bad;    /* step1 自环已注释；恢复时此行可删 */

    uart_printf("[LCD] init ST7789P3 240x320, SPI1 PA4(CS)/PA5(SCK)/PA7(MOSI)"
                " DC=PC3 RST=PA0, VCC=2.8V(pin9)\r\n");

#if PSG_TEST_LCD_WIRING
    /* 第 0 步：接线自检 —— 万用表直流挡在 FPC 侧逐根量（对 GND）：
     *   pin5 CS=0V  pin3 SCK=0V  pin6 SDA=3.3V  pin4 RS=3.3V  pin2 RESET=3.3V
     * 不符的那根线就是断路/接错；全反 = FPC 方向拿反了（查 pin1 标记）。
     * 本步停在此处不返回；量完把 test_config.h 的 PSG_TEST_LCD_WIRING 置 0 */
    uart_printf("[LCD] step0: wiring check - 万用表直流挡量 FPC 侧(对GND):\r\n");
    uart_printf("       pin5 CS=0V  pin3 SCK=0V  pin6 SDA=3.3V"
                "  pin4 RS=3.3V  pin2 RESET=3.3V\r\n");
    bsp_lcd_wiring_check();
    while (1)
    {
    }
#endif

    bsp_lcd_init();

    /* 设置方向并取逻辑宽高：之后所有坐标都在这个坐标系里 */
    bsp_lcd_set_rotation(LCD_TEST_ROT);
    w = bsp_lcd_width();
    h = bsp_lcd_height();
    uart_printf("[LCD] rotation=%d -> %ux%u logical\r\n",
                (unsigned)LCD_TEST_ROT, (unsigned)w, (unsigned)h);

    /* 第 1 步：SPI1 自环 —— 证明 SPI1 配置和驱动本身没问题。
     * 前提：断电状态下用杜邦线短接 PA7(MOSI) <-> PA6(MISO) */
    // uart_printf("[LCD] step1: SPI1 loopback (PA7<->PA6 短接了吗?)...\r\n");
    // bad = bsp_lcd_spi_loopback();
    // if (bad == 0)
    // {
    //     uart_printf("[LCD] step1 PASS: SPI1 收发自回全对\r\n");
    // }
    // else
    // {
    //     uart_printf("[LCD] step1 FAIL: %d 字节不符 —— SPI1/驱动有问题，"
    //                 "后面的测试无意义，先修这里\r\n", bad);
    //     TEST_ASSERT(0);
    //     return;
    // }
    // uart_printf("[LCD] 拔掉 PA7-PA6 短接线，再跑下一轮（否则读回全是回显）\r\n");

    /* 第 2 步：读回诊断 —— RDDID(0x04)/RDDST(0x09)，各 1 个哑元字节。
     * 前提：已拔短接线，模组 SDO pin7 -> PA6 */
    bsp_lcd_read_reg(0x04, 1, id, 3);   //读取芯片的ID
    uart_printf("[LCD] RDDID = %x %x %x\r\n", id[0], id[1], id[2]);
    bsp_lcd_read_reg(0x09, 1, id, 4); //读取芯片的运行状态
    uart_printf("[LCD] RDDST = %x %x %x %x \r\n",
                id[0], id[1], id[2], id[3]);

    uart_printf("[LCD] filling solid colors...\r\n");
    for (i = 0; i < sizeof(colors) / sizeof(colors[0]); i++)
    {
        bsp_lcd_fill(0, 0, w - 1, h - 1, colors[i]);
        lcd_test_delay_ms(600);
    }

    /* 8 根沿宽度方向的色条：同时验证几何和颜色 */
    uart_printf("[LCD] drawing color bars (should stay on screen)\r\n");
    for (i = 0; i < 8; i++)
    {
        uint16_t x0 = (uint16_t)(i * w / 8);
        uint16_t x1 = (uint16_t)((i + 1) * w / 8 - 1);

        bsp_lcd_fill(x0, 0, x1, h - 1, colors[i]);
    }

    /* 走到这里说明 SPI 时序没有卡死/超时；显示效果靠人眼判定 */
    TEST_ASSERT(1); 

    uart_printf("[LCD] done.\r\n");
    while(1)
    {
        
    }
}

#endif /* PSG_TEST_BSP_LCD */
