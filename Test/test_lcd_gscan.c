/*********************************************************************
 * test_lcd_gscan.c - GSCAN(0x45) 扫描行读取验证 + 扫描时序标定
 *
 * 要回答的四个问题（结果直接喂给下一阶段的软件同步设计）：
 *   1. 命令正确性：这颗 ST7789P3 克隆片支不支持 0x45？dummy/高低字节
 *      理解对不对？（P0/P1）
 *   2. 位置可读性：能否连续读到走动的扫描行？回绕周期 = 面板真实
 *      刷新周期 T_scan？（P2）
 *   3. 轮询频率：各读速档下单次读耗时、可持续采样率，最快可信档
 *      是哪档？（P3）
 *   4. 位置准确性：行号 vs 时间线性回归斜率 与 周期反推斜率 自洽？
 *      （P2/P4 三条证据链：斜率自洽 + 帧内线性 + 跨速一致）
 *
 * 形态：调度器前裸机上下文（本用例是上电第一个碰屏的，跑完 t4 再
 * init 屏，互不影响）；mcycle 时标；只读命令不写面板 → 零残留。
 *
 * 本阶段只测量，不做同步/防撕裂（回绕点与可见帧头的绝对相位关系
 * 纯读无法证明，标注待验证，下阶段翻色标定）。
 *
 * 待板验清单（P5 静态分析的边界）：
 *   - ML 位(MADCTL bit4)对 gate 扫描方向的实际影响
 *   - GSCAN 计数是否含 porch 行（决定回绕点偏移）
 *   - 回绕时刻 = 可见帧头？（需人工翻色观察）
 *********************************************************************/
#include "test_config.h"   /* 开关必须先包含再 #if，写反整文件编译为空 */

#if PSG_TEST_LCD_GSCAN

#include <stdint.h>
#include <string.h>
#include "test_framework.h"
#include "bsp_lcd.h"
#include "bsp_uart.h"
#include "ch32v4x7.h"          /* SPI_BaudRatePrescaler_x 宏 */
#include "system_ch32v4x7.h"   /* SystemCoreClock：tick↔µs 换算跟随主频 */

#define GSCAN_CMD      0x45    /* GSCAN：读当前扫描行（2 参数：高8位、低8位） */
#define GSCAN_DUMMY    1

/* mcycle CSR 读数（CPU 周期计数器；调度器前无干扰，量程 28.6s@150M） */
static inline uint32_t cyc(void)
{
    uint32_t v;
    __asm volatile ("csrr %0, mcycle" : "=r" (v));
    return v;
}

static void delay_ticks(uint32_t ticks)
{
    uint32_t t0 = cyc();
    while ((cyc() - t0) < ticks) { }
}

static uint32_t ticks_to_us(uint32_t t)
{
    return (uint32_t)(((uint64_t)t * 1000000ULL) / SystemCoreClock);
}

/* 单次 GSCAN 读取，时间戳取事务中点（首尾各有半次事务的相位误差，
 * 对回归斜率无影响，对绝对相位有 ±半次读耗的误差——频率梯里量化它） */
static uint16_t gscan_rd(uint16_t presc, uint32_t *t_mid)
{
    uint8_t  b[2];
    uint32_t t0 = cyc();

    (void)bsp_lcd_read_reg_p(GSCAN_CMD, GSCAN_DUMMY, b, 2, presc);
    *t_mid = t0 + ((cyc() - t0) >> 1);
    return (uint16_t)(((uint16_t)b[0] << 8) | b[1]);
}

/* ==================== 连续采样引擎（在线统计，不存原始样本） ====================
 * 样本流 → 回绕检测 + 量程 + 异常计数 + 段内线性回归（回绕之间算一段，
 * 段内以相对时间为自变量控制乘积量级，全部 uint64 累加器） */
typedef struct {
    uint32_t wraps;                 /* 回绕次数（行号大幅回落 ≥128 判一次） */
    uint32_t t_wrap[16];            /* 前 16 次回绕时刻（算周期用） */
    uint32_t max_val, min_val;      /* 全程量程（判行/半行单位） */
    uint32_t cnt_ff;                /* 0xFFFF（SDO 悬空特征） */
    uint32_t cnt_jump;              /* 帧内异常跳增（≥65 行/样本间隔） */
    uint32_t cnt_neg;               /* 小幅回落（-127..-2）：不算失败，观察用 */
    uint32_t seg_n;                 /* 当前段样本数 */
    uint32_t seg_t0;                /* 当前段时间起点 */
    uint64_t st, sv, stv, st2;      /* Σt,Σv,Σtv,Σt² */
    uint32_t seg_cnt;               /* 已结算的段数 */
    uint64_t slope_sum;             /* 各段斜率累计（行/us ×1e6） */
    uint32_t slope_min, slope_max;
    uint8_t  have_prev;
    uint32_t prev_v;
} burst_t;

static burst_t s_b;                 /* 静态省主栈（~120B bss） */

static void burst_reset(burst_t *b)
{
    memset(b, 0, sizeof(*b));
    b->min_val = 0xFFFFFFFFu;
    b->slope_min = 0xFFFFFFFFu;
}

/* 段结算：最小二乘斜率 = (nΣtv−ΣtΣv)/(nΣt²−(Σt)²)，行/tick × MHz → 行/us
 * 定点 1e6。真实斜率 ~0.01/us，先乘后除会被取整成 0（见函数内注释） */
static void burst_seg_close(burst_t *b)
{
    if (b->seg_n >= 16u)
    {
        int64_t num = (int64_t)((uint64_t)b->seg_n * b->stv - b->st * b->sv);
        int64_t den = (int64_t)((uint64_t)b->seg_n * b->st2 - b->st * b->st);

        if ((num > 0) && (den > 1000000))
        {
            /* sl = 斜率×1e6（行/us 定点）。直接 num×MHz/den ≈ 0.01 会被取整成 0
             * （板证 2026-10-16：段斜率全 0 的根因）；den 先除 1e6 降位再乘，
             * num×MHz ≤ ~1.5e16 仍在 uint64，保留 ~10 位有效数字 */
            uint32_t sl = (uint32_t)(((uint64_t)num
                          * (uint64_t)(SystemCoreClock / 1000000u))
                          / ((uint64_t)den / 1000000ULL));

            b->slope_sum += sl;
            if (sl < b->slope_min) { b->slope_min = sl; }
            if (sl > b->slope_max) { b->slope_max = sl; }
            b->seg_cnt++;
        }
    }
    b->seg_n = 0;
    b->st = b->sv = b->stv = b->st2 = 0;
}

static void burst_feed(burst_t *b, uint32_t v, uint32_t t)
{
    if (v == 0xFFFFu)               /* SDO 悬空/读失败特征：不进统计 */
    {
        b->cnt_ff++;
        return;
    }

    if (v > b->max_val) { b->max_val = v; }
    if (v < b->min_val) { b->min_val = v; }

    if (b->have_prev != 0)
    {
        int32_t d = (int32_t)v - (int32_t)b->prev_v;

        if (d <= -128)              /* 大幅回落 = 扫描行回绕 */
        {
            if (b->wraps < 16u) { b->t_wrap[b->wraps] = t; }
            b->wraps++;
            burst_seg_close(b);     /* 上一段结算，本样本开新段 */
        }
        else if (d >= 65)           /* 一次读间隔(≤35µs≈0.8行)不可能跳这么多 */
        {
            b->cnt_jump++;
        }
        else if (d <= -2)           /* 小幅回落：计数观察，不算异常 */
        {
            b->cnt_neg++;
        }
    }

    if (b->seg_n == 0u) { b->seg_t0 = t; }
    {
        uint32_t tr = t - b->seg_t0;

        b->st  += tr;
        b->sv  += v;
        b->stv += (uint64_t)tr * (uint64_t)v;
        b->st2 += (uint64_t)tr * (uint64_t)tr;
        b->seg_n++;
    }

    b->prev_v = v;
    b->have_prev = 1;
}

/* 采到 need_wraps 次回绕或超时 */
static void burst_run(burst_t *b, uint16_t presc, uint32_t need_wraps,
                      uint32_t timeout_ms)
{
    uint32_t t_start = cyc();
    uint32_t t_lim = (uint32_t)(((uint64_t)timeout_ms * SystemCoreClock) / 1000u);

    burst_reset(b);
    for (;;)
    {
        uint32_t t;
        uint16_t v = gscan_rd(presc, &t);

        burst_feed(b, v, t);
        if (b->wraps >= need_wraps)    { break; }
        if ((cyc() - t_start) > t_lim) { break; }
    }
}

/* 相邻回绕时刻差 → 平均/最小/最大周期（µs）。回绕 <2 次返回 0 */
static uint32_t burst_tscan_us(const burst_t *b, uint32_t *min_us, uint32_t *max_us)
{
    uint32_t k = b->wraps - 1u;
    uint32_t sum = 0, mn = 0xFFFFFFFFu, mx = 0;

    if (b->wraps < 2u) { return 0; }
    if (k > 15u) { k = 15u; }          /* t_wrap 只存前 16 个 */
    for (uint32_t i = 0; i < k; i++)
    {
        uint32_t d = b->t_wrap[i + 1] - b->t_wrap[i];

        sum += d;
        if (d < mn) { mn = d; }
        if (d > mx) { mx = d; }
    }
    *min_us = ticks_to_us(mn);
    *max_us = ticks_to_us(mx);
    return ticks_to_us(sum / k);
}

/* buf 内 len 字节是否全为 val */
static int all_is(const uint8_t *buf, uint8_t len, uint8_t val)
{
    for (uint8_t i = 0; i < len; i++)
    {
        if (buf[i] != val) { return 0; }
    }
    return 1;
}

void test_lcd_gscan(void)
{
    static uint16_t vals[32];        /* P1 样本（静态省栈） */
    uint8_t  buf[4];
    uint32_t tscan_us, tmin_us = 0, tmax_us = 0;
    uint32_t range, ref_slope, fit_slope, dev_pct;

    /* 本用例是本次上电第一个碰屏的；后面 t4 的 port init 会再 init 一遍，
     * 重跑初始化序列无害（test_bsp_lcd 时代就是这个顺序） */
    bsp_lcd_init();

    /* ---------- P0 读通路自检：MISO 路径没坏才有后面的一切 ---------- */
    {
        uint8_t n1 = bsp_lcd_read_reg(0x04, 1, buf, 3);        /* RDDID */

        uart_printf("gscan: P0 RDDID(%uB)=%02x %02x %02x",
                    (unsigned)n1, buf[0], buf[1], buf[2]);
        TEST_ASSERT(!all_is(buf, 3, 0xFF));                    /* 全FF=SDO悬空 */
        (void)bsp_lcd_read_reg(0x09, 1, buf, 4);               /* RDDST：板证过的通路 */
        uart_printf(" RDDST=%02x %02x %02x %02x\r\n",
                    buf[0], buf[1], buf[2], buf[3]);
        TEST_ASSERT(!all_is(buf, 4, 0xFF));
    }

    /* ---------- P1 应答门：0x45 有应答、数值在走 ---------- */
    {
        uint32_t distinct = 0, cnt_ff = 0;

        for (uint32_t i = 0; i < 32u; i++)
        {
            uint32_t t;

            vals[i] = gscan_rd(SPI_BaudRatePrescaler_128, &t);
            if (vals[i] == 0xFFFFu) { cnt_ff++; }
            delay_ticks(SystemCoreClock / 1000u);              /* 1ms 间隔 */
        }
        for (uint32_t i = 0; i < 32u; i++)                     /* O(n²) 查不同值个数 */
        {
            uint32_t j;
            for (j = 0; j < i; j++)
            {
                if (vals[j] == vals[i]) { break; }
            }
            if (j == i) { distinct++; }
        }
        uart_printf("gscan: P1 应答 前8值 %04x %04x %04x %04x %04x %04x %04x %04x"
                    " 不同值%lu FF=%lu\r\n",
                    vals[0], vals[1], vals[2], vals[3],
                    vals[4], vals[5], vals[6], vals[7],
                    (unsigned long)distinct, (unsigned long)cnt_ff);
        TEST_ASSERT(distinct >= 2);          /* 数值在走（死了=恒定值） */
        TEST_ASSERT(cnt_ff <= 4);            /* 偶发可忍，成片 FF = 没接好 */

        /* dummy 对照：dummy=0 时读到的是(哑元字节,参数高8位)。哑元=0 的话
         * 值恒 ≤0x00FF；dummy=1 正确时值域是完整行号 → 两组量程分开即证明 */
        uint32_t v1_min = 0xFFFFu, v1_max = 0, v0_min = 0xFFFFu, v0_max = 0;

        for (uint32_t i = 0; i < 8u; i++)
        {
            uint32_t t;
            uint16_t v1 = gscan_rd(SPI_BaudRatePrescaler_128, &t);
            uint16_t v0;

            (void)bsp_lcd_read_reg_p(GSCAN_CMD, 0, buf, 2, SPI_BaudRatePrescaler_128);
            v0 = (uint16_t)(((uint16_t)buf[0] << 8) | buf[1]);
            if (v1 < v1_min) { v1_min = v1; }
            if (v1 > v1_max) { v1_max = v1; }
            if (v0 < v0_min) { v0_min = v0; }
            if (v0 > v0_max) { v0_max = v0; }
        }
        uart_printf("gscan: P1 dummy对照 d=1[%04x..%04x] d=0[%04x..%04x]"
                    "（板证 d=0 高8位=哑元字节0xff/不定→量程错位，d=1 才是真行号）\r\n",
                    (unsigned)v1_min, (unsigned)v1_max,
                    (unsigned)v0_min, (unsigned)v0_max);
    }

    /* ---------- P2 参考 burst：/128 安全档，8 回绕 / 300ms ---------- */
    {
        burst_run(&s_b, SPI_BaudRatePrescaler_128, 8, 300);
        tscan_us = burst_tscan_us(&s_b, &tmin_us, &tmax_us);
        range = (s_b.min_val <= s_b.max_val) ? (s_b.max_val - s_b.min_val) : 0;

        uart_printf("gscan: P2 锯齿 回绕×%lu 量程[%04lx..%04lx] "
                    "T_scan=%luus(%lu.%luHz) min/max=%lu/%lu\r\n",
                    (unsigned long)s_b.wraps,
                    (unsigned long)s_b.min_val, (unsigned long)s_b.max_val,
                    (unsigned long)tscan_us,
                    (unsigned long)(1000000UL / tscan_us),
                    (unsigned long)((10000000UL / tscan_us) % 10UL),
                    (unsigned long)tmin_us, (unsigned long)tmax_us);

        TEST_ASSERT(s_b.wraps >= 4);                 /* 锯齿成立 */
        TEST_ASSERT((tscan_us >= 5000) && (tscan_us <= 40000));  /* 25~200Hz 面板常识窗 */
        /* 板证 2026-10-16：~4000 样本偶发 1 次读毛刺（P1 首样 0000 同款）。
         * 单点毛刺不破坏回绕检测（间隔全在 16723..16747）；同步环将来对
         * 可疑值双读/中值滤波即可，这里容忍 ≤2 */
        TEST_ASSERT(s_b.cnt_jump <= 2);
        TEST_ASSERT_EQ_INT(0, s_b.cnt_ff);
    }

    /* ---------- P4a 斜率自洽（回归斜率 vs 周期反推斜率） ---------- */
    {
        /* 周期反推：量程/T_scan = 平均斜率（行/us ×1e6 定点） */
        ref_slope = (tscan_us != 0)
                    ? (uint32_t)(((uint64_t)range * 1000000ULL) / tscan_us) : 0;
        fit_slope = (s_b.seg_cnt != 0)
                    ? (uint32_t)(s_b.slope_sum / s_b.seg_cnt) : 0;
        dev_pct = (ref_slope != 0)
                  ? (uint32_t)((fit_slope > ref_slope)
                      ? ((uint64_t)(fit_slope - ref_slope) * 100ULL / ref_slope)
                      : ((uint64_t)(ref_slope - fit_slope) * 100ULL / ref_slope)) : 100;

        /* 展示换算成 计数/ms（内部 1e6 定点是 行/us；÷1000 → ms 定点 1e3，
         * 量级 ~9.9 不会被两位小数吃成 0 —— 板证第一版按/us 打印全是 0.00） */
        uart_printf("gscan: P4 斜率 回归=%lu.%02lu 反推=%lu.%02lu 计数/ms 偏差%lu%%"
                    " 段数%lu(段间散布 %lu..%lu) 小回落%lu\r\n",
                    (unsigned long)(fit_slope / 1000UL),
                    (unsigned long)((fit_slope % 1000UL) / 10UL),
                    (unsigned long)(ref_slope / 1000UL),
                    (unsigned long)((ref_slope % 1000UL) / 10UL),
                    (unsigned long)dev_pct,
                    (unsigned long)s_b.seg_cnt,
                    (unsigned long)s_b.slope_min, (unsigned long)s_b.slope_max,
                    (unsigned long)s_b.cnt_neg);
        TEST_ASSERT(dev_pct <= 10);                  /* 自洽 = 读的是实时位置 */
    }

    /* ---------- P3 频率梯：/128 /64 /32 /16，单次耗时 + 该档 T_scan ---------- */
    {
        static const uint16_t presc_tab[4] = {
            SPI_BaudRatePrescaler_128, SPI_BaudRatePrescaler_64,
            SPI_BaudRatePrescaler_32,  SPI_BaudRatePrescaler_16
        };
        static const char *name_tab[4] = { "/128", "/64", "/32", "/16" };
        uint8_t fastest = 0xFF;                      /* 最快可信档（倒序选） */

        uart_printf("gscan: P3 频率梯（对照 /128 的 T_scan=%luus）\r\n",
                    (unsigned long)tscan_us);
        for (uint32_t k = 0; k < 4u; k++)
        {
            uint32_t sum = 0, mn = 0xFFFFFFFFu, mx = 0;
            uint32_t t2, dmin = 0, dmax = 0, dev2;
            uint32_t anoms;

            for (uint32_t i = 0; i < 200u; i++)      /* 单次读耗时（含换分频/CS 开销） */
            {
                uint32_t t0 = cyc();

                (void)gscan_rd(presc_tab[k], &t2);
                {
                    uint32_t d = cyc() - t0;

                    sum += d;
                    if (d < mn) { mn = d; }
                    if (d > mx) { mx = d; }
                }
            }
            burst_run(&s_b, presc_tab[k], 4, 150);   /* 该档独立测 T_scan */
            t2 = burst_tscan_us(&s_b, &dmin, &dmax);
            anoms = s_b.cnt_ff + s_b.cnt_jump;
            dev2 = ((t2 != 0) && (tscan_us != 0) && (t2 > tscan_us))
                 ? (uint32_t)(((uint64_t)(t2 - tscan_us) * 100ULL) / tscan_us)
                 : (((t2 != 0) && (tscan_us != 0))
                     ? (uint32_t)(((uint64_t)(tscan_us - t2) * 100ULL) / tscan_us) : 100);
            {
                uint32_t avg_us = ticks_to_us(sum / 200u);

                /* uart_printf 不支持 %-4s（无 '-' flag），只认 %s ——
                 * 板证第一版按 %-4s 打印整行参数错位全乱 */
                uart_printf("gscan:   %s 单次avg %luus(min %lu max %lu)"
                            " ≈%lukHz | T=%luus 回绕%lu 异常%lu 偏差%lu%% %s\r\n",
                            name_tab[k],
                            (unsigned long)avg_us,
                            (unsigned long)ticks_to_us(mn),
                            (unsigned long)ticks_to_us(mx),
                            (unsigned long)(avg_us != 0
                                ? 1000000UL / avg_us / 1000UL : 0),
                            (unsigned long)t2, (unsigned long)s_b.wraps,
                            (unsigned long)anoms, (unsigned long)dev2,
                            (anoms <= 2 && s_b.wraps >= 4 && dev2 <= 3)
                                ? "可信" : "弃用");
            }
            /* 表序慢→快：取"最后一个"可信档才是最快（板证第一版带
             * fastest==0xFF 门，结果永远选中第一个=/128 最慢档） */
            if ((anoms <= 2) && (s_b.wraps >= 4) && (dev2 <= 3))
            {
                fastest = (uint8_t)k;
            }
        }
        uart_printf("gscan: P3 最快可信档 = %s\r\n",
                    (fastest == 0xFF) ? "无（/128 也不可信，查接线/面板）"
                                      : name_tab[fastest]);
        TEST_ASSERT(fastest != 0xFF);
    }

    /* ---------- P2b 漂移复测：1 秒后再测一轮，T_scan 稳不稳 ---------- */
    {
        uint32_t t2, dmin = 0, dmax = 0;

        delay_ticks(SystemCoreClock / 2u);           /* 0.5s 间隔 */
        burst_run(&s_b, SPI_BaudRatePrescaler_128, 4, 200);
        t2 = burst_tscan_us(&s_b, &dmin, &dmax);
        {
            uint32_t drift = (t2 > tscan_us)
                ? (uint32_t)(((uint64_t)(t2 - tscan_us) * 100ULL) / tscan_us)
                : (uint32_t)(((uint64_t)(tscan_us - t2) * 100ULL) / tscan_us);

            uart_printf("gscan: P2b 漂移 0.5s后 T=%luus 变化%lu.%lu%%\r\n",
                        (unsigned long)t2,
                        (unsigned long)(drift / 10UL), (unsigned long)(drift % 10UL));
            TEST_ASSERT(drift <= 50);                /* ≤5%：决定同步环可否离线标定 */
        }
    }

    /* ---------- P5 扫描方向静态分析 + 结论 ---------- */
    {
        uart_printf("gscan: P5 MADCTL=0x60(MV|MX,ML=0) → 扫描轴=玻璃Y(0..319)"
                    "=横屏逻辑U(左→右)，逻辑V=239-玻璃X\r\n");
        uart_printf("gscan: P5 待板验：ML位影响/GSCAN含porch行?/回绕点=帧头?"
                    "（下阶段回绕时刻翻色人工标定）\r\n");

        if ((range >= 280) && (range <= 360))
        {
            uart_printf("gscan: 结论 单位=行(量程%lu) T_scan=%luus —— "
                        "软件同步的三个参数齐了（单位/周期/最快档）\r\n",
                        (unsigned long)range, (unsigned long)tscan_us);
        }
        else if ((range >= 560) && (range <= 720))
        {
            uart_printf("gscan: 结论 单位=半行(量程%lu，÷2折算行号) "
                        "T_scan=%luus —— 参数齐了\r\n",
                        (unsigned long)range, (unsigned long)tscan_us);
        }
        else if ((range >= 150) && (range <= 180))
        {
            /* 板证 2026-10-16 实测量程 166：(320 可见 + ~13 porch) / 2 ≈ 166.5
             * → 疑似 2 gate 行/计数；绝对映射下阶段回绕时刻翻色标定 */
            uart_printf("gscan: 结论 单位=疑似2行/计数(量程%lu ≈ (320+porch)/2) "
                        "T_scan=%luus —— 周期/量程已实测，映射待翻色标定\r\n",
                        (unsigned long)range, (unsigned long)tscan_us);
        }
        else
        {
            uart_printf("gscan: 结论 量程%lu 非典型(320/640)，按实测折算单位，"
                        "T_scan=%luus\r\n",
                        (unsigned long)range, (unsigned long)tscan_us);
        }
    }
}

#endif /* PSG_TEST_LCD_GSCAN */
