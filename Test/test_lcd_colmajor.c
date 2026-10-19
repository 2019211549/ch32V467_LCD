/*********************************************************************
 * test_lcd_colmajor.c - 横屏 + DMA 按列发送（列优先写 GRAM 演示 + 翻色实验）
 *
 * 模式（2026-10-19 板证收官）：MADCTL=0x40（MV=0，板证黑带上/红端左
 * 两判据全过），横屏画面方向与 0x60 完全一致；每板 = 转置窗口
 * (CASET=v 全跨 0..239 作快轴, RASET=u 的 16 列) + 列连续缓冲
 * (第 j 列 240px 连续) + 一笔 3840 半字异步 DMA。
 *
 * 阶段：
 *   演示  慢扫一遍（每板 1s，串口报板号）：看写前沿沿 u 左->右逐板推进
 *   翻色  GSCAN 回绕对齐 + 红蓝整屏翻转（第二轮主实验）：
 *         ① δ0 细钉梯 {1120,1180,1240}µs 钉 Δ；② 节奏梯（δ0=0）：
 *         pace {T/20+8, T/20, 背靠背}；每档 ~4s
 *
 * ===== 第一轮板证（2026-10-19，δ0 梯 {0,600,900,1100,1300,1600,2100}）=====
 *  - δ0<1300 各档：屏幕有一条红/蓝竖向分界线，δ0 越大线越靠左；
 *    δ0=1300 起无线，干净（跨帧翻色）。原理（追逐模型，板证反推）：
 *    写前沿每列 ~53µs、扫前沿每列 ~50µs（GSCAN 量程 334 行含 ~14 行
 *    porch，可见行率 T_scan/334 而非 T_scan/320）→ 扫描每列追回
 *    s≈3.5~4µs，在第 c_x=(Δ-δ0)/s 列追上写：左边"写完才显示"=同帧
 *    新色，右边"显示时写未到"=本帧旧色 → 分界线即同帧/跨帧分界。
 *    δ0 增大 → 领先 (Δ-δ0) 缩小 → c_x 左移；δ0>=Δ 后扫描全程领先 → 线
 *    消失。Δ 由两法独立测得一致（行优先反解 1165 / 本轮区间 (1100,1300]）。
 *  - 对比行优先 δ0*=1950=785+Δ：列优先把 785（扫描渡越一整板）项
 *    消掉了 → 安全线塌缩到 Δ 附近。"写入粒度决定撕裂条件"的板证。
 *  - 预测教训：纸面推 s≈0.5µs/列（以为 δ0=0 干净），实测 s≈3.5-4 ——
 *    写物理下限 51.8µs/列可能永远追不上扫描 ~50.1 → 同帧翻色或不可达。
 *
 * ===== 第二轮（本版）：① δ0 细钉梯 {1120,1180,1240} 钉 Δ 到 ±60µs；
 * ② 节奏裁剪梯（δ0=0）：pace {T/20+8, T/20, 背靠背} —— 线若右移但
 * 不消失 => 扫描快于 DMA 极限、同帧不可达、生产最优 δ0>=Δ；线若消失
 * => 写速能压过扫描，δ0=0 同帧翻色成立。
 *
 * 独立：不碰 PSRAM/LVGL；GSCAN 需 SDO 接好，读不到回绕则跳过翻色段。
 *********************************************************************/
#include "test_config.h"   /* 开关必须先包含再 #if，写反整文件编译为空 */

#if PSG_TEST_LCD_COLMAJOR

#include <stdint.h>
#include "test_framework.h"
#include "bsp_lcd.h"
#include "bsp_uart.h"
#include "ch32v4x7.h"          /* SPI_BaudRatePrescaler_x 宏 */
#include "system_ch32v4x7.h"   /* SystemCoreClock：tick↔µs 换算跟随主频 */

/* MV=0 族正解（2026-10-19 板证定案 = 理论推导值）：0x40 板测
 * 黑带在上(MX 对) + 红端在左(MY 对) -> 屏上u=RASET值、屏上v=CASET值
 * 自然升序，慢扫左->右。曾误读板照改 0xC0，用户更正后回改 */
#define CM_MADCTL_OK    0x40u

#define CM_DMA_GUARD    8000000UL    /* 单板等 DMA 完成超时 ~53ms@150M */
#define CM_BLACK_ROWS   24u          /* 每列前 24 像素写黑：顶带=MX 判据 */
#define CM_SLOW_TICKS   SystemCoreClock           /* 演示板间停顿 1s */

#define CM_GSCAN_CMD    0x45
#define CM_RD_PRESC     SPI_BaudRatePrescaler_16  /* 板证最快可信档 5µs/次 */
#define CM_COLOR_A      0xF800u                    /* 红 */
#define CM_COLOR_B      0x001Fu                    /* 蓝 */
#define CM_WRAP_DROP    (-128)                     /* 回绕判据：计数大幅回落 */
#define CM_FLIP_STEP_S  4u                         /* 每个 δ0 档翻转时长 */

/* 第二轮 δ0 细钉梯：一轮板证 1100 有线 / 1300 无线 → Δ∈(1100,1300]，
 * 这三档把 Δ 钉到 ±60µs（最后一档仍有线的位置 ≈ Δ−s×屏宽内左移量） */
static const uint16_t cm_d0_tab[] = { 1120, 1180, 1240 };

/* 16 级色带（u 向彩虹，判据：红端应在左） */
static const uint16_t cm_ramp[16] = {
    LCD_RED,     0xF9A0, 0xFCE0, 0xF7E0, 0xB7E0, 0x9FE0, LCD_GREEN,
    0x05E0, 0x03E0, 0x01E0, 0x07FF, 0x041F, 0x001F, 0x3BFF, LCD_MAGENTA,
    LCD_WHITE,
};

static uint16_t s_slab[16u * 240u] __attribute__((aligned(4)));  /* 列连续板 */

static inline uint32_t cm_cyc(void)
{
    uint32_t v;
    __asm volatile ("csrr %0, mcycle" : "=r" (v));
    return v;
}

static void cm_delay_ticks(uint32_t ticks)
{
    uint32_t t0 = cm_cyc();
    while ((cm_cyc() - t0) < ticks) { }
}

static uint32_t cm_ticks_to_us(uint32_t t)
{
    return (uint32_t)(((uint64_t)t * 1000000ull) / SystemCoreClock);
}

static uint32_t cm_us_to_ticks(uint32_t us)
{
    return us * (SystemCoreClock / 1000000u);
}

/* ---------------- GSCAN 回绕（flipcal 同款） ---------------- */
static uint16_t cm_gscan(uint32_t *t_mid)
{
    uint8_t  b[2];
    uint32_t t0 = cm_cyc();

    (void)bsp_lcd_read_reg_p(CM_GSCAN_CMD, 1, b, 2, CM_RD_PRESC);
    *t_mid = t0 + ((cm_cyc() - t0) >> 1);   /* 事务中点作时间戳 */
    return (uint16_t)(((uint16_t)b[0] << 8) | b[1]);
}

/* 等回绕：返回检出时刻(tick)，超时返回 0。检出精度 ~5µs */
static uint32_t cm_wait_wrap(uint32_t timeout_ms)
{
    uint32_t t0 = cm_cyc();
    uint32_t lim = timeout_ms * (SystemCoreClock / 1000u);
    uint32_t t;
    uint16_t prev = cm_gscan(&t);

    for (;;)
    {
        uint16_t v = cm_gscan(&t);

        if ((int32_t)v - (int32_t)prev <= CM_WRAP_DROP) { return t; }
        prev = v;
        if ((cm_cyc() - t0) > lim) { return 0; }
    }
}

/* 回绕周期 T_scan（一次间隔测量；0=无回绕=SDO 不在线） */
static uint32_t cm_measure_tscan(void)
{
    uint32_t t1 = cm_wait_wrap(60);
    uint32_t t2;

    if (t1 == 0u) { return 0u; }
    t2 = cm_wait_wrap(60);
    if (t2 == 0u) { return 0u; }
    return cm_ticks_to_us(t2 - t1);
}

/* ---------------- 演示：慢扫一遍（定向观感） ---------------- */
static uint32_t cm_slab_sweep(uint32_t pace_ticks)
{
    uint32_t k, j, v;

    for (k = 0; k < 20u; k++)
    {
        uint32_t done0 = 0, done = 0, guard;

        bsp_lcd_set_window(0, 16u * k, 239, 16u * k + 15u);
        for (j = 0; j < 16u; j++)
        {
            uint16_t c = cm_ramp[(16u * k + j) / 20u];
            for (v = 0; v < 240u; v++)
            {
                s_slab[j * 240u + v] = (v < CM_BLACK_ROWS) ? LCD_BLACK : c;
            }
        }

        bsp_lcd_dma_stats(0, &done0, 0);
        (void)bsp_lcd_write_pixels_async(s_slab, 16u * 240u);
        guard = CM_DMA_GUARD;
        for (;;)
        {
            bsp_lcd_dma_stats(0, &done, 0);
            if (done != done0) { break; }
            if (--guard == 0u)
            {
                bsp_lcd_dma_recover();
                break;
            }
        }

        uart_printf("colmajor: 板 %lu/20  u=%lu..%lu 已上图\r\n",
                    (unsigned long)(k + 1u),
                    (unsigned long)(16u * k), (unsigned long)(16u * k + 15u));
        if (pace_ticks != 0u)
        {
            cm_delay_ticks(pace_ticks);
        }
    }
    return 0;
}

/* ---------------- 翻色实验：列优先追扫整屏翻一色 ----------------
 * 回绕检出 -> δ0 起写延迟 -> 20 板按 pace 配速发射（板 k 截止
 * = 起写 + k×pace，与行优先生产同款节拍；差别只在窗口转置+列缓冲）。
 * 返回 0 正常 / 1 无回绕 / 2 板 DMA 超时（已 recover） */
static int cm_flip_screen(uint32_t delta_us, uint32_t pace_us)
{
    uint32_t t_wrap = cm_wait_wrap(50);
    uint32_t t0, k, done0, done, guard;

    if (t_wrap == 0u) { return 1; }
    while ((cm_cyc() - t_wrap) < cm_us_to_ticks(delta_us)) { }
    t0 = cm_cyc();
    for (k = 0; k < 20u; k++)
    {
        while ((cm_cyc() - t0) < cm_us_to_ticks(pace_us * k)) { }

        bsp_lcd_dma_stats(0, &done0, 0);
        bsp_lcd_set_window(0, 16u * k, 239, 16u * k + 15u);
        (void)bsp_lcd_write_pixels_async(s_slab, 16u * 240u);
        guard = CM_DMA_GUARD;
        for (;;)
        {
            bsp_lcd_dma_stats(0, &done, 0);
            if (done != done0) { break; }
            if (--guard == 0u)
            {
                bsp_lcd_dma_recover();
                return 2;
            }
        }
    }
    return 0;
}

static void cm_fill_solid(uint16_t color)
{
    uint32_t i;
    for (i = 0; i < (sizeof(s_slab) / sizeof(s_slab[0])); i++)
    {
        s_slab[i] = color;
    }
}

/* δ0 阶梯主流程。返回 1 = GSCAN 不在线（整段跳过） */
static int cm_flip_ladder(uint32_t tscan_us)
{
    uint32_t pace_us = tscan_us / 20u + 8u;
    uint8_t si;

    uart_printf("colmajor: 翻色实验：T_scan=%luus pace=%luus/板 "
                "(板=16列,~52.8us/列 vs 扫描可见行率 ~50.1)\r\n",
                (unsigned long)tscan_us, (unsigned long)pace_us);

    for (si = 0; si < (sizeof(cm_d0_tab) / sizeof(cm_d0_tab[0])); si++)
    {
        uint32_t d0 = cm_d0_tab[si];
        uint32_t t_step = cm_cyc();
        uint16_t color = CM_COLOR_A;
        uint32_t flips = 0, rc;

        uart_printf("colmajor: --- δ0 细钉 档%lu  d0=%luus ---\r\n",
                    (unsigned long)(si + 1u), (unsigned long)d0);
        do
        {
            cm_fill_solid(color);
            rc = (uint32_t)cm_flip_screen(d0, pace_us);
            if (rc != 0u)
            {
                uart_printf("colmajor: flip rc=%lu（1=无回绕 2=DMA超时），"
                            "阶梯中止\r\n", (unsigned long)rc);
                return (rc == 1u) ? 1 : 0;
            }
            flips++;
            color = (color == CM_COLOR_A) ? CM_COLOR_B : CM_COLOR_A;
        } while ((cm_cyc() - t_step) < CM_FLIP_STEP_S * SystemCoreClock);

        uart_printf("colmajor: 档%lu 完成 %lu 次翻转\r\n",
                    (unsigned long)(si + 1u), (unsigned long)flips);
    }
    return 0;
}

/* 节奏裁剪梯（δ0=0）：pace 依次 T/20+8 / T/20 / 背靠背（0=只受 DMA+
 * 窗口开销限制的物理极限）。看分界线右移还是消失：
 *   右移但不消失 => 扫描可见行率快于写物理极限，同帧翻色不可达
 *   消失         => 写速能压过扫描，δ0=0 同帧干净翻色成立 */
static int cm_pace_ladder(uint32_t tscan_us)
{
    const uint32_t ptab[3] = { tscan_us / 20u + 8u, tscan_us / 20u, 0u };
    uint8_t si;

    for (si = 0; si < 3u; si++)
    {
        uint32_t t_step = cm_cyc();
        uint16_t color = CM_COLOR_A;
        uint32_t flips = 0, rc;

        uart_printf("colmajor: --- 节奏档%lu  pace=%luus/板 (d0=0) %s ---\r\n",
                    (unsigned long)(si + 1u), (unsigned long)ptab[si],
                    (si == 2u) ? "背靠背物理极限" : "");
        do
        {
            cm_fill_solid(color);
            rc = (uint32_t)cm_flip_screen(0u, ptab[si]);
            if (rc != 0u)
            {
                uart_printf("colmajor: flip rc=%lu，节奏梯中止\r\n",
                            (unsigned long)rc);
                return (rc == 1u) ? 1 : 0;
            }
            flips++;
            color = (color == CM_COLOR_A) ? CM_COLOR_B : CM_COLOR_A;
        } while ((cm_cyc() - t_step) < CM_FLIP_STEP_S * SystemCoreClock);

        uart_printf("colmajor: 节奏档%lu 完成 %lu 次翻转\r\n",
                    (unsigned long)(si + 1u), (unsigned long)flips);
    }
    return 0;
}

void test_lcd_colmajor(void)
{
    uint32_t st = 0, dn = 0, er = 0, fb0, tscan_us;

    bsp_lcd_init();
    bsp_lcd_set_rotation(LCD_ROT_LANDSCAPE);   /* 标准初始化（0x60）走一遍 */
    bsp_lcd_dma_async_enable(0);   /* TCIE+空回调：裸机轮询 done 计数（同 flipcal） */
    fb0 = bsp_lcd_dma_fallback_cnt();

    uart_printf("\r\n=== 列优先 DMA（横屏不变，MADCTL=0x%02X）===\r\n",
                CM_MADCTL_OK);

    /* 演示段：慢扫一遍（写前沿沿 u 左->右，与扫描同向） */
    uart_printf("colmajor: 慢扫演示：每板停 1s，跟串口板号看屏\r\n");
    bsp_lcd_set_madctl_raw(CM_MADCTL_OK);
    (void)cm_slab_sweep(CM_SLOW_TICKS);

    /* 翻色段：第二轮两梯 */
    tscan_us = cm_measure_tscan();
    if (tscan_us == 0u)
    {
        uart_printf("colmajor: 无 GSCAN 回绕（SDO 不在线？），翻色段跳过\r\n");
    }
    else
    {
        int skip = cm_flip_ladder(tscan_us);

        uart_printf("colmajor: δ0 细钉梯完毕。请上报：三档各有无线、"
                    "线的位置（最后一档仍有线 => Δ>1240）\r\n");
        if (skip == 0)
        {
            skip = cm_pace_ladder(tscan_us);
        }
        if (skip == 0)
        {
            uart_printf("colmajor: 节奏梯完毕。请上报背靠背档：线右移不消失"
                        "（同帧不可达）/ 线消失（δ0=0 同帧成立）\r\n");
        }
    }

    bsp_lcd_dma_stats(&st, &dn, &er);
    uart_printf("colmajor: dma %lu/%lu err=%lu fb=%lu\r\n",
                (unsigned long)st, (unsigned long)dn, (unsigned long)er,
                (unsigned long)(bsp_lcd_dma_fallback_cnt() - fb0));
    TEST_ASSERT_EQ_INT(st, dn);
    TEST_ASSERT_EQ_INT(er, 0);
}

#endif /* PSG_TEST_LCD_COLMAJOR */
