/*********************************************************************
 * test_bsp_psram.c - 片上 PSRAM 独立单元测试（2026-10-16，基线 7cdb267 上）
 *
 * 定位：【完全独立】只依赖 bsp_psram 驱动（EVT 例程同款 300M 配置），
 * 不动 Link.ld / psg_config.h / LVGL。基线上没有别人用 PSRAM，
 * 整个 4MB 窗口随便写，测完不留状态（下次上电即净）。
 *
 * 测什么（调度器前裸机上下文，阻塞几十 ms）：
 *   1. bsp_psram_init()：EVT 初始化 + 内部自检（走步 + 粗地址检查）
 *   2. 三区间图案扫描：起始 64KB（i^0x5A5A 遍历全部 65536 个 16 位值，
 *      数据线粘 0/1 全覆盖）/ 中部 8KB / 尾部 8KB，失配打印期望/实读
 *   3. 地址线别名自检：全片 2^n 偏移各写唯一标记，全写完再全读回
 *      （高位地址线粘连 = 两个偏移落同一格，先写的标记被覆盖而现形）
 *   4. mcycle 计时 64KB 写/读带宽（只打印不断言：整帧 DIRECT 需求
 *      >30MB/s，带宽容忍度大）
 *
 * init FAIL 也【不提前退出】：继续裸探测并打印失配形态，对照定位：
 *   实读 = 前一个字的值   -> 读延迟档偏低（数据晚一拍）
 *   固定位错（如 bit3 恒0）-> 数据线粘 0/1
 *   只有高偏移区间失配     -> 高位地址线问题
 *   随机散点失配           -> 时序临界（trc），按 bsp_psram.c 头部梯子降档
 *********************************************************************/
#include "test_config.h"   /* 开关必须先包含再 #if，写反整文件编译为空 */

#if PSG_TEST_BSP_PSRAM

#include <stdint.h>
#include "test_framework.h"
#include "bsp_psram.h"
#include "bsp_uart.h"
#include "system_ch32v4x7.h"     /* SystemCoreClock：带宽换算跟随主频预设 */

/* 起始区 64KB 扫描（32K 半字）；中部/尾部各 8KB（4K 半字） */
#define TST_MAIN_HALFWORDS  (32u * 1024u)
#define TST_SMALL_HALFWORDS (4u * 1024u)

/* mcycle CSR 读数（CPU 周期计数器，调度器前后都能用；核心时钟下 32 位
 * ≥14s 回绕，本测试毫秒级量程安全。计数率=SystemCoreClock） */
static inline uint32_t cyc(void)
{
    uint32_t v;
    __asm volatile ("csrr %0, mcycle" : "=r"(v));
    return v;
}

/* 区间扫描：[off, off+cnt) 半字写 (i ^ pat) 再回读核对。
 * 返回失配半字数；前 3 处打印绝对地址/期望/实读 */
static uint32_t probe_region(uint32_t off, uint32_t cnt, uint16_t pat)
{
    volatile uint16_t *p =
        (volatile uint16_t *)(BSP_PSRAM_WINDOW_BASE + off * 2u);
    uint32_t i, bad = 0;

    for (i = 0; i < cnt; i++)
    {
        p[i] = (uint16_t)(i ^ pat);
    }
    for (i = 0; i < cnt; i++)
    {
        uint16_t v = p[i];

        if (v != (uint16_t)(i ^ pat))
        {
            bad++;
            if (bad <= 3u)
            {
                uart_printf("psram:   失配 @%08lx: 期望 %04x 实读 %04x\r\n",
                            (unsigned long)(BSP_PSRAM_WINDOW_BASE + (off + i) * 2u),
                            (unsigned)(uint16_t)(i ^ pat), (unsigned)v);
            }
        }
    }
    return bad;
}

/* 地址线别名自检：0、512B、1K...2M（2^n 共 13 点）+ 最高地址，各写
 * 唯一标记 0xB00x，全写完再全读回。返回失配点数（失配即疑似地址线粘连） */
static uint32_t alias_check(void)
{
    uint32_t off[16];
    uint32_t n = 0, step, i, bad = 0;

    off[n++] = 0;
    for (step = 512u; step < BSP_PSRAM_SIZE; step <<= 1u)
    {
        off[n++] = step;
    }
    off[n++] = BSP_PSRAM_SIZE - 2u;

    for (i = 0; i < n; i++)
    {
        *(volatile uint16_t *)(BSP_PSRAM_WINDOW_BASE + off[i]) =
            (uint16_t)(0xB000u | (uint16_t)i);
    }
    for (i = 0; i < n; i++)
    {
        uint16_t v = *(volatile uint16_t *)(BSP_PSRAM_WINDOW_BASE + off[i]);

        if (v != (uint16_t)(0xB000u | (uint16_t)i))
        {
            bad++;
            if (bad <= 3u)
            {
                uart_printf("psram:   别名失配 @%08lx: 期望 %04x 实读 %04x"
                            "（地址线疑似粘连）\r\n",
                            (unsigned long)(BSP_PSRAM_WINDOW_BASE + off[i]),
                            (unsigned)(0xB000u | i), (unsigned)v);
            }
        }
    }
    return bad;
}

void test_bsp_psram(void)
{
    volatile uint16_t *p = (volatile uint16_t *)BSP_PSRAM_WINDOW_BASE;
    volatile uint32_t sink = 0;         /* 防读循环被优化掉 */
    uint32_t t0, t1, t2;
    uint32_t i, bad, bad_total = 0;
    int init_ok;

    /* 1. EVT 序列初始化（幂等）+ 内部自检 */
    init_ok = (bsp_psram_init() == 0);
    uart_printf("psram: init %s（EVT序列, trc=0x14, 300M档+SYSCLK300M）\r\n",
                (init_ok != 0) ? "OK" : "FAIL");
    TEST_ASSERT_EQ_INT(1, init_ok);     /* FAIL 也继续：失配形态是定位关键 */

    /* 2. 三区间图案扫描（数据线覆盖 + 区间定位） */
    uart_printf("psram: 区间扫描（起始64KB 中部8KB 尾部8KB）\r\n");
    bad = probe_region(0, TST_MAIN_HALFWORDS, 0x5A5Au);
    uart_printf("psram:   起始 64KB: 失配 %lu\r\n", (unsigned long)bad);
    bad_total += bad;

    bad = probe_region(BSP_PSRAM_SIZE / 4u, TST_SMALL_HALFWORDS, 0x33CCu);
    uart_printf("psram:   中部 8KB: 失配 %lu\r\n", (unsigned long)bad);
    bad_total += bad;

    bad = probe_region((BSP_PSRAM_SIZE / 2u) - TST_SMALL_HALFWORDS,
                       TST_SMALL_HALFWORDS, 0x0F0Fu);
    uart_printf("psram:   尾部 8KB: 失配 %lu\r\n", (unsigned long)bad);
    bad_total += bad;

    /* 3. 地址线别名自检 */
    bad = alias_check();
    uart_printf("psram: 地址线别名自检（全片 15 点）: 失配 %lu\r\n",
                (unsigned long)bad);
    bad_total += bad;

    /* 4. 带宽粗测（整数运算：MB/s = 字节数 x MHz / 周期数。
     * MHz 用 SystemCoreClock 换算——跟随主频预设自动适配，不再写死） */
    t0 = cyc();
    for (i = 0; i < TST_MAIN_HALFWORDS; i++)
    {
        p[i] = (uint16_t)(i ^ 0x5A5Au);
    }
    t1 = cyc();
    for (i = 0; i < TST_MAIN_HALFWORDS; i++)
    {
        sink += p[i];
    }
    t2 = cyc();
    (void)sink;
    uart_printf("psram: 64KB 写 %luMB/s 读 %luMB/s（AN参考 写~130 读~111）\r\n",
                (unsigned long)((TST_MAIN_HALFWORDS * 2u * (SystemCoreClock / 1000000u)) / (t1 - t0)),
                (unsigned long)((TST_MAIN_HALFWORDS * 2u * (SystemCoreClock / 1000000u)) / (t2 - t1)));

    /* 结论行（runner 的 [OK]/[FAIL] 由断言计数决定，这里给人看） */
    TEST_ASSERT_EQ_INT(0, bad_total);
    if ((init_ok != 0) && (bad_total == 0u))
    {
        uart_printf("psram: 结论 PASS —— 4MB 数据/地址线全对，PSRAM 可用\r\n");
    }
    else
    {
        uart_printf("psram: 结论 FAIL（init=%d 数据失配 %lu）——按失配形态"
                    " 对照文件头四类定位；300M 仍失配则按 bsp_psram.c 头部"
                    " 梯子降档（下一档 240M 预设+225M 行；再试 CFIFO→WRADD）\r\n",
                    init_ok, (unsigned long)bad_total);
    }
}

#endif /* PSG_TEST_BSP_PSRAM */
