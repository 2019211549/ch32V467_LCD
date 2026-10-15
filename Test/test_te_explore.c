/*********************************************************************
 * test_te_explore.c - TE（撕裂效应）命令探索（硬件在环，2026-10-15）
 *
 * 背景：ST7789 支持 TE 输出（TEOFF 0x34 / TEON 0x35 / SETSCR 0x44），
 * 但本模组 P032B017P 的 FPC 没焊 TE 引脚（pin15~18 NC）——TE 是纯电平
 * 输出，命令发了也只打在没接的 pad 上，示波器/万用表永远测不到。
 *
 * 那这个实验还能"看到"什么：TE 模式的开/关状态记录在 RDDST(0x09)
 * 状态字里，走 SDO(pin7)→PA6 可以读回来。本用例的玩法：
 *   1. 基线 RDDST（此时 TE 关，上电默认）
 *   2. TEON(仅V-blanking) → RDDST，看哪几个字节翻
 *   3. TEON(V+H blanking) → RDDST，看模式位进一步变化
 *   4. SETSCR(第 80 行) → RDDST（该命令无回读寄存器，纯发送看总线不死）
 *   5. TEOFF → RDDST，应回到基线
 *
 * 判读：哪几个字节变化由人工看串口输出（ST7789P3 与 ST7789V 手册的
 * 位定义可能有出入，不做固定位断言，只断言读回机制本身工作）。
 * 板证 2026-10-15：b2=92<->93 随 TEON/TEOFF 干净翻转（使能位实锤）；
 * TEON(v+h) 独有 b3=0x10（模式位）；TEOFF 复原 b2 但 b3.4 残留（模式值
 * 粘性，位命名待 P3 手册）。ST7789V 手册"TE 位在第 4 字节高 3 位"与
 * P3 实测不符，以实测为准。
 *
 * 依赖 SDO 接好（同 read_reg 路径）。纯驱动层实验，不需要 LVGL/调度器，
 * 在 runner 阶段直接跑；跑完 TEOFF 恢复干净状态（T3 随后还会整体重新 init）。
 *********************************************************************/
#include "test_config.h"   /* 开关必须先包含再 #if，写反整文件编译为空 */

#if PSG_TEST_TE_EXPLORE

#include "test_framework.h"
#include "bsp_lcd.h"
#include "bsp_uart.h"

/* 读 4 字节 RDDST 并打印；tag 标注当前 TE 状态，diff 逐字节对比基线。
 * 板证教训（2026-10-15）：uart_printf 不支持 %-14s 这类左对齐标志——
 * 不认识的规格会【原样输出且不消费参数】，导致下一个 %02x 吃掉 tag 的
 * 指针、打印出 flash 地址（地址步进=字面量长度对齐，铁证）。自制
 * formatter 只支持"宽度+0填充"，%-s 一律别用 */
static void te_snap(const char *tag, const uint8_t *base)
{
    uint8_t b[4] = {0, 0, 0, 0};
    uint8_t i;

    (void)bsp_lcd_read_reg(0x09, 1, b, sizeof(b));   /* RDDST：1 dummy + 4 数据 */
    uart_printf("te %s: RDDST=%02x %02x %02x %02x", tag, b[0], b[1], b[2], b[3]);

    if (base != 0)
    {
        uart_printf("  diff:");
        for (i = 0; i < 4; i++)
        {
            if (b[i] != base[i])
            {
                uart_printf(" [%u]%02x->%02x", (unsigned)i, base[i], b[i]);
            }
        }
    }
    uart_printf("\r\n");
}

void test_te_explore(void)
{
    uint8_t base[4] = {0, 0, 0, 0};
    uint8_t n;

    bsp_lcd_init();                          /* 纯驱动层：GPIO/SPI/面板 init 序列 */

    n = bsp_lcd_read_reg(0x09, 1, base, sizeof(base));
    TEST_ASSERT_EQ_INT(n, 4);                /* 读回机制必须工作，否则后面全白搭 */
    te_snap("baseline", 0);

    bsp_lcd_te_on(0);                        /* TEON 模式0：仅 V-blanking */
    te_snap("TEON(v-only)", base);

    bsp_lcd_te_on(1);                        /* TEON 模式1：V+H blanking */
    te_snap("TEON(v+h)", base);

    bsp_lcd_te_set_scanline(80);             /* SETSCR：TE 脉冲挪到第 80 行 */
    te_snap("SETSCR(80)", base);

    bsp_lcd_te_off();                        /* TEOFF：应回到基线 */
    te_snap("TEOFF", base);

    uart_printf("te: 若 diff 列只在 TEON/TEOFF 步骤出现且 TEOFF 无 diff，"
                "说明命令已进面板（只是 pad 没接）\r\n");
}

#endif /* PSG_TEST_TE_EXPLORE */
