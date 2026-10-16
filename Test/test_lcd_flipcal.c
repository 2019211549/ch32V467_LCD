/*********************************************************************
 * test_lcd_flipcal.c - GSCAN 回绕点↔可见帧头 翻色标定（光学观察实验）
 *
 * 板证结论（2026-10-16 前两轮）：
 *   - 0x45 有效：量程 0..166 疑似 2 gate 行/计数，T_scan≈16.74ms/59.7Hz
 *   - 物理扫描 = 横屏左→右（横带写入的撕裂线"左上→右下"唯一匹配此假设）
 *   - 横带（垂直于扫描轴）写入必然产生斜向撕裂线 v=0.764u —— 已光学证实
 *
 * 本轮（第三轮板证后的修正）：16 列竖板同向追扫。板证教训——起写贴着
 * 回绕、写速≈扫速的"贴身追"仍是斜纹（每板一条陡斜线 左上→右下）：
 * 写一块板要 819µs，期间扫描已走过 ~15.6 列；板内数据序是列内自上而
 * 下，扫到板左列时整列旧、扫到板右列时"上新下旧"，分界高度逐列 +15px
 * → 每板一条陡斜线。正确条件是"写一块板的整段时间不得与扫描在该板
 * 内重叠"，即写要落后扫描渡越一整块板的时间（785µs=16×52.33）再加
 * 相位偏移 Δ：
 *   - 起写延迟 δ0 ≥ 785 + Δ + 余量；板周期 pace = T_scan/20+8 ≈ 845µs
 *     （略慢于扫描 837µs，落后量只增不减）
 *   - 板证 2026-10-16 第四轮：δ0=1650 左上3齿、1750 左上2齿，齿随 δ0
 *     增大左移缩小（方向符合理论；每+100µs约少1齿，比理论的 7.7µs/板
 *     间距宽——按经验值走）
 *   - 板证收官（第五轮）：1850有齿 / 1950·2050干净 / B@2000整屏零齿
 *     → δ0*∈(1850,1950]，Δ = δ0*−785 ≈ 1.1ms（≈21列 porch），标定完结
 *
 * 写路径：
 *   阶段A 横带基线 = bsp_lcd_fill 轮询（对照用，斜线形态已知）
 *   阶段B/C 竖板 = 16列×240行 乒乓双缓冲 SRAM 条带 + 每板窗口 + 异步
 *   DMA（生产目标形态；纯色两板内容相同，乒乓只为演示生产节拍——
 *   LVGL 接入后 gather 重打包进这两块板）。
 *
 * 调度器前裸机：DMA 完成等不来信号量（阻塞语义只在任务态成立），
 * 开 TCIE + NULL 回调 + 轮询 done 计数；之后 T3 的 port 会重注册回调。
 *
 * 观察方式：眼看 / 手机慢动作。全程 ~15s，跑完屏恢复单色零残留。
 *********************************************************************/
#include "test_config.h"   /* 开关必须先包含再 #if，写反整文件编译为空 */

#if PSG_TEST_LCD_FLIPCAL

#include <stdint.h>
#include "test_framework.h"
#include "bsp_lcd.h"
#include "bsp_uart.h"
#include "ch32v4x7.h"          /* SPI_BaudRatePrescaler_x 宏 */
#include "system_ch32v4x7.h"   /* SystemCoreClock：tick↔µs 换算跟随主频 */

#define FC_GSCAN_CMD   0x45
#define FC_RD_PRESC    SPI_BaudRatePrescaler_16   /* P3 板证最快可信档 5µs/次 */
#define FC_COLOR_A     0xF800u                     /* 红 */
#define FC_COLOR_B     0x001Fu                     /* 蓝 */
#define FC_WRAP_DROP   (-128)                      /* 回绕判据：计数大幅回落 */
#define FC_DMA_GUARD   8000000UL                   /* 单板等完成超时 ~53ms@150M */
#define FC_SLAB_W      16u                         /* 板宽（列数） */
#define FC_TRAIL_US    785u    /* 扫描渡越一块板=16×52.33µs：起写必须落后≥此值+Δ */
#define FC_D0_SAFE     2100u   /* 生产起写延迟 = δ0*+150（板证 δ0*=1950，
                                  1850仍有齿；安全窗 [1950,~16000]，取2100） */

/* 16列×240行 乒乓双板：2×7.68K。生产形态下 LVGL 横带 gather 进这里 */
static uint16_t s_slab[2][FC_SLAB_W * 240u];
static uint16_t s_slab_color = 0xFFFFu;   /* 0xFFFF 不在用色表里=必重填 */
static uint32_t s_fc_dma_to;              /* 板 DMA 等完成超时次数 */

static inline uint32_t fc_cyc(void)
{
    uint32_t v;
    __asm volatile ("csrr %0, mcycle" : "=r" (v));
    return v;
}

static void fc_delay_ticks(uint32_t ticks)
{
    uint32_t t0 = fc_cyc();
    while ((fc_cyc() - t0) < ticks) { }
}

static uint32_t fc_ticks_to_us(uint32_t t)
{
    return (uint32_t)(((uint64_t)t * 1000000ULL) / SystemCoreClock);
}

static uint32_t fc_us_to_ticks(uint32_t us)
{
    return us * (SystemCoreClock / 1000000u);
}

static uint16_t fc_gscan(uint32_t *t_mid)
{
    uint8_t  b[2];
    uint32_t t0 = fc_cyc();

    (void)bsp_lcd_read_reg_p(FC_GSCAN_CMD, 1, b, 2, FC_RD_PRESC);
    *t_mid = t0 + ((fc_cyc() - t0) >> 1);   /* 事务中点作时间戳 */
    return (uint16_t)(((uint16_t)b[0] << 8) | b[1]);
}

/* 等回绕：返回检出时刻(tick)，超时返回 0。检出精度 ~5µs（轮询周期） */
static uint32_t fc_wait_wrap(uint32_t timeout_ms)
{
    uint32_t t0 = fc_cyc();
    uint32_t lim = timeout_ms * (SystemCoreClock / 1000u);
    uint32_t t;
    uint16_t prev = fc_gscan(&t);

    for (;;)
    {
        uint16_t v = fc_gscan(&t);

        if ((int32_t)v - (int32_t)prev <= FC_WRAP_DROP) { return t; }
        prev = v;
        if ((fc_cyc() - t0) > lim) { return 0; }
    }
}

/* 乒乓板着色（换色才重填，两板都填） */
static void fc_slab_prepare(uint16_t color)
{
    if (color == s_slab_color) { return; }
    for (uint32_t b = 0; b < 2u; b++)
    {
        for (uint32_t i = 0; i < (sizeof(s_slab[0]) / sizeof(s_slab[0][0])); i++)
        {
            s_slab[b][i] = color;
        }
    }
    s_slab_color = color;
}

/* 发射第 k 板：竖窗口 + 异步 DMA + 轮询 done 等收尾。
 * 返回 0 正常 / 1 超时（已 recover，调用方中止余下板） */
static int fc_slab_launch(uint32_t k)
{
    uint32_t done0;

    bsp_lcd_dma_stats(0, &done0, 0);
    bsp_lcd_set_window((uint16_t)(k * FC_SLAB_W), 0,
                       (uint16_t)(k * FC_SLAB_W + FC_SLAB_W - 1u),
                       bsp_lcd_height() - 1u);
    (void)bsp_lcd_write_pixels_async(s_slab[k & 1u],
                                     FC_SLAB_W * bsp_lcd_height());
    {
        uint32_t guard = FC_DMA_GUARD;
        uint32_t done;

        for (;;)
        {
            bsp_lcd_dma_stats(0, &done, 0);
            if (done > done0) { return 0; }
            if (--guard == 0)
            {
                bsp_lcd_dma_recover();
                s_fc_dma_to++;
                return 1;
            }
        }
    }
}

/* 竖板整屏写：20 板按序发射。
 *   paced=0 满速连发（测 T_fill）；paced=1 追扫配速（板 k 截止=起写+k×pace）
 *   delta_us：回绕检出后的起写延迟（阶梯用）
 * 返回整屏耗时 µs；t0_out/t_end_out 可传出起写/收笔时刻（tick） */
static uint32_t fc_slab_screen(int paced, uint32_t delta_us, uint32_t pace_us,
                               uint32_t t_wrap, uint32_t *t0_out,
                               uint32_t *t_end_out)
{
    uint32_t n = bsp_lcd_width() / FC_SLAB_W;    /* 20 */
    uint32_t t0;

    while ((fc_cyc() - t_wrap) < fc_us_to_ticks(delta_us)) { }
    t0 = fc_cyc();
    for (uint32_t k = 0; k < n; k++)
    {
        if (paced != 0)
        {
            while ((fc_cyc() - t0) < fc_us_to_ticks(pace_us * k)) { }
        }
        if (fc_slab_launch(k) != 0) { break; }   /* DMA 超时：中止余下板 */
    }
    if (t0_out != 0)   { *t0_out = t0; }
    if (t_end_out != 0) { *t_end_out = fc_cyc(); }
    return fc_ticks_to_us(fc_cyc() - t0);
}

void test_lcd_flipcal(void)
{
    uint32_t tscan_us = 0, pace_us, tfill_us = 0;

    bsp_lcd_init();
    bsp_lcd_set_rotation(LCD_ROT_LANDSCAPE);   /* 产品方向：横屏 MADCTL=0x60 */

    /* 开 DMA 完成中断 + 空回调：裸机态靠 ISR 收尾（CS 释放/DFF 复位/
     * done 计数）；后面 T3 的 port 会重注册自己的回调 */
    bsp_lcd_dma_async_enable(0);

    /* ---------- 预备：T_scan + 竖板满速 T_fill ---------- */
    {
        uint32_t tw[5];
        uint8_t  ok = 1;

        for (uint8_t i = 0; i < 5u; i++)
        {
            tw[i] = fc_wait_wrap(50);
            if (tw[i] == 0) { ok = 0; break; }
        }
        TEST_ASSERT(ok);
        if (ok)
        {
            for (uint8_t i = 0; i < 4u; i++)
            {
                tscan_us += fc_ticks_to_us(tw[i + 1] - tw[i]);
            }
            tscan_us /= 4u;
        }
        pace_us = (tscan_us / (bsp_lcd_width() / FC_SLAB_W)) + 8u;

        fc_slab_prepare(FC_COLOR_A);
        {
            uint32_t t_det = fc_wait_wrap(50);

            (void)fc_slab_screen(0, 0, pace_us, t_det, 0, 0);   /* 冷启动 */
            t_det = fc_wait_wrap(50);
            tfill_us = fc_slab_screen(0, 0, pace_us, t_det, 0, 0);
        }
        {
            uint32_t st = 0, dn = 0, er = 0;

            bsp_lcd_dma_stats(&st, &dn, &er);
            uart_printf("flipcal: 预备 T_scan=%luus 竖板满速T_fill=%luus(%u板x%u列"
                        " 板均%luus) 配速pace=%luus DMA %lu/%lu/%lu\r\n",
                        (unsigned long)tscan_us, (unsigned long)tfill_us,
                        (unsigned)(bsp_lcd_width() / FC_SLAB_W),
                        (unsigned)FC_SLAB_W,
                        (unsigned long)(tfill_us / (bsp_lcd_width() / FC_SLAB_W)),
                        (unsigned long)pace_us,
                        (unsigned long)st, (unsigned long)dn, (unsigned long)er);
        }
        TEST_ASSERT((tscan_us > 8000) && (tscan_us < 40000));   /* 25~125Hz 常识窗 */
        TEST_ASSERT((tfill_us > 14000) && (tfill_us < 19000));  /* 20板满速≈16.5ms */
    }

    /* ---------- 阶段A：横带回绕对齐连翻 4s（斜线基线，对照用） ---------- */
    uart_printf("flipcal: === 阶段A 横带基线 4s（预期：斜线 左上→右下）===\r\n");
    {
        uint32_t t_start = fc_cyc();
        uint32_t flips = 0, timeouts = 0;
        uint32_t fill_sum = 0;
        uint16_t color = FC_COLOR_B;

        bsp_lcd_fill(0, 0, bsp_lcd_width() - 1u, bsp_lcd_height() - 1u,
                     FC_COLOR_A);
        while ((fc_cyc() - t_start) < 4u * SystemCoreClock)
        {
            uint32_t t_det = fc_wait_wrap(50);

            if (t_det == 0) { timeouts++; continue; }
            color = (color == FC_COLOR_A) ? FC_COLOR_B : FC_COLOR_A;
            {
                uint32_t t0 = fc_cyc();

                bsp_lcd_fill(0, 0, bsp_lcd_width() - 1u,
                             bsp_lcd_height() - 1u, color);
                fill_sum += fc_ticks_to_us(fc_cyc() - t0);
            }
            flips++;
        }
        if (flips != 0)
        {
            uart_printf("flipcal: 阶段A 翻转%lu次 整屏写avg=%luus 超时%lu\r\n",
                        (unsigned long)flips,
                        (unsigned long)(fill_sum / flips),
                        (unsigned long)timeouts);
        }
        TEST_ASSERT(flips >= 200);
        TEST_ASSERT_EQ_INT(0, timeouts);
    }

    /* ---------- 阶段B：竖板追扫 δ0=1650 连翻 4s（零撕裂验证） ---------- */
    uart_printf("flipcal: === 阶段B 竖板追扫δ0=%uus 4s ===\r\n",
                (unsigned)FC_D0_SAFE);
    uart_printf("flipcal: 看：预期整屏干净红蓝交替（无斜纹无竖线）；\r\n"
                "flipcal:       若仍有齿，数齿数+记位置（应只剩左上角）\r\n");
    {
        uint32_t t_start = fc_cyc();
        uint32_t flips = 0, timeouts = 0;
        uint32_t chase_sum = 0;
        uint16_t color = FC_COLOR_B;

        fc_slab_prepare(FC_COLOR_A);
        (void)fc_slab_screen(1, FC_D0_SAFE, pace_us, fc_wait_wrap(50), 0, 0);
        while ((fc_cyc() - t_start) < 4u * SystemCoreClock)
        {
            uint32_t t_det = fc_wait_wrap(50);

            if (t_det == 0) { timeouts++; continue; }
            color = (color == FC_COLOR_A) ? FC_COLOR_B : FC_COLOR_A;
            fc_slab_prepare(color);
            chase_sum += fc_slab_screen(1, FC_D0_SAFE, pace_us, t_det, 0, 0);
            flips++;
        }
        if (flips != 0)
        {
            uart_printf("flipcal: 阶段B 翻转%lu次 追扫整屏avg=%luus 超时%lu"
                        " DMA超时%lu\r\n",
                        (unsigned long)flips,
                        (unsigned long)(chase_sum / flips),
                        (unsigned long)timeouts,
                        (unsigned long)s_fc_dma_to);
        }
        TEST_ASSERT(flips >= 100);      /* 追扫跨 2 帧，周期 33.5ms ≈ 119 翻 */
        TEST_ASSERT_EQ_INT(0, timeouts);
        TEST_ASSERT_EQ_INT(0, s_fc_dma_to);
    }

    /* ---------- 阶段C：δ0 细阶梯（钉死 δ0*：齿数=0 的最小档） ---------- */
    uart_printf("flipcal: === 阶段C δ0细阶梯（每档~1.6s 红蓝连翻）===\r\n");
    uart_printf("flipcal: 看：逐档数左上角齿数（0/1/2/3..）；\r\n"
                "flipcal:       齿数=0的最小档=δ0*，生产参数取 δ0*+150us\r\n");
    {
        static const uint16_t d_tab[4] = { 1750, 1850, 1950, 2050 };

        for (uint8_t k = 0; k < 4u; k++)
        {
            uart_printf("flipcal: --- 档%u δ0=%uus ---\r\n",
                        (unsigned)k, (unsigned)d_tab[k]);
            fc_delay_ticks(SystemCoreClock / 4u);          /* 250ms 静屏 */
            for (uint8_t i = 0; i < 48u; i++)              /* 48×33.5ms≈1.6s */
            {
                uint32_t t_det = fc_wait_wrap(50);
                uint16_t color = (i & 1u) ? FC_COLOR_A : FC_COLOR_B;

                if (t_det == 0) { break; }
                fc_slab_prepare(color);
                (void)fc_slab_screen(1, d_tab[k], pace_us, t_det, 0, 0);
            }
        }
        fc_slab_prepare(FC_COLOR_A);
        (void)fc_slab_screen(1, FC_D0_SAFE, pace_us, fc_wait_wrap(50), 0, 0);
        uart_printf("flipcal: 阶段C 完。发回：B@2000齿数 / C各档齿数(如3/2/1/0)"
                    " —— 齿数=0最小档=δ0*\r\n");
    }

    {
        uint32_t st = 0, dn = 0, er = 0;

        bsp_lcd_dma_stats(&st, &dn, &er);
        TEST_ASSERT_EQ_INT(0, er);                          /* DMA 全程无错 */
        TEST_ASSERT_EQ_INT(0, bsp_lcd_dma_fallback_cnt());  /* 恒走异步 */
    }
}

#endif /* PSG_TEST_LCD_FLIPCAL */
