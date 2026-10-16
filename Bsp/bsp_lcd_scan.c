/*********************************************************************
 * bsp_lcd_scan.c - GSCAN 扫描同步 + 16 列竖板追扫写板器（生产路径）
 *
 * 全部参数的板证出处：Test/test_lcd_flipcal.c（2026-10-16 标定收官）
 *   T_scan=16747µs(59.7Hz)  扫描=横屏左→右 52.33µs/列
 *   回绕早于可见帧头 Δ≈1.1ms  零撕裂阈值 δ0*=1950µs（1850 仍有齿）
 * 追扫的几何原理（为什么必须落后一整块板）：
 *   写一块板 819µs ≈ 扫描渡越一块板 785~837µs —— 起写若贴着扫描，
 *   扫描会在板内撞见"写一半"（列内自上而下写 → 上新下旧的齿）；
 *   唯有 δ0 ≥ 785+Δ 让写整段落在"扫描已离开本板、下一趟还没回来"
 *   的盲窗里，每列被采样时要么全旧要么全新 → 整屏在趟边界统一翻转。
 *
 * 内存布局：
 *   staging：PSRAM 数据窗口 +0x80000（512K 处），一帧 320x240x2=150K。
 *   挑这个偏移是避开 T4 整帧实验的两块帧缓冲（窗口 0..0x4B000）；
 *   4MB 窗口里两者永久共存，谁开谁用
 *   s_slab：SRAM 乒乓板 2x(16x240x2)=15K，gather 的落点、DMA 的源
 *
 * 已知时序账（横屏 320x240，板证数值）：
 *   整屏追扫 = δ0 2100 + 首板 gather ~0.4ms + 20 板 x845 ≈ 19.7ms < 2 帧
 *   → 全屏更新周期 2 帧（33.5ms，~30fps 上限）；这是"零撕裂"的代价，
 *   静态界面无脏区不追扫，不受此限
 *********************************************************************/
#include "bsp_lcd_scan.h"
#include "bsp_lcd.h"
#include "bsp_psram.h"
#include "psg_config.h"     /* PSG_LOG */
#include "ch32v4x7.h"       /* SPI_BaudRatePrescaler_16 */
#include "system_ch32v4x7.h" /* SystemCoreClock：tick↔µs 换算跟随主频 */
#include <string.h>         /* memset staging */

#define SC_GSCAN_CMD   0x45
#define SC_RD_PRESC    SPI_BaudRatePrescaler_16   /* 板证最快可信读档 5µs/次 */
#define SC_WRAP_DROP   (-128)      /* 回绕判据：计数大幅回落 */
#define SC_DMA_GUARD   12000000UL  /* 单板等完成超时 ~40ms@300M */
#define SC_SLAB_W      16u         /* 板宽（列数）= 标定实验同款 */
#define SC_D0_US       2100u       /* 起写延迟 = 板证 δ0*1950 + 余量 */
#define SC_STAGE_OFF   0x00080000UL /* staging 偏移：避开 T4 两块整帧 */

/* SRAM 乒乓板：gather 落点/DMA 源。DMA 写板 k 时 gather 板 k+1 进另一块 */
static uint16_t s_slab[2][SC_SLAB_W * 240u] __attribute__((aligned(4)));

static uint16_t *s_stage;          /* PSRAM 整帧 staging 基址 */
static volatile uint8_t s_ready;
static uint16_t  s_stride;         /* 行距（半字）= 逻辑宽 320 */
static uint32_t  s_nslab;          /* 板数 = 320/16 = 20 */
static uint32_t  s_tscan_us;       /* 实测扫描周期 */
static uint32_t  s_pace_us;        /* 板配速 = T_scan/20 + 8 */

static uint32_t  s_frames;         /* 追扫帧数 */
static uint64_t  s_sum_us;         /* 追扫耗时累计（求平均） */
static uint32_t  s_max_us;
static uint64_t  s_gather_sum;     /* 每帧 gather 耗时累计（求平均） */
static uint32_t  s_wrap_to, s_dma_to;

/* ==================== 时间基（mcycle，同 flipcal） ==================== */

static inline uint32_t sc_cyc(void)
{
    uint32_t v;
    __asm volatile ("csrr %0, mcycle" : "=r" (v));
    return v;
}

static uint32_t sc_ticks_to_us(uint32_t t)
{
    return (uint32_t)(((uint64_t)t * 1000000ULL) / SystemCoreClock);
}

static uint32_t sc_us_to_ticks(uint32_t us)
{
    return us * (SystemCoreClock / 1000000u);
}

/* ==================== GSCAN 读取（0x45，只读命令零残留） ==================== */

static uint16_t sc_gscan(uint32_t *t_mid)
{
    uint8_t  b[2];
    uint32_t t0 = sc_cyc();

    (void)bsp_lcd_read_reg_p(SC_GSCAN_CMD, 1, b, 2, SC_RD_PRESC);
    *t_mid = t0 + ((sc_cyc() - t0) >> 1);   /* 事务中点作时间戳 */
    return (uint16_t)(((uint16_t)b[0] << 8) | b[1]);
}

/* 等回绕：返回检出时刻(tick)，超时返回 0。检出精度 ~5µs（轮询周期） */
static uint32_t sc_wait_wrap(uint32_t timeout_ms)
{
    uint32_t t0 = sc_cyc();
    uint32_t lim = timeout_ms * (SystemCoreClock / 1000u);
    uint32_t t;
    uint16_t prev = sc_gscan(&t);

    for (;;)
    {
        uint16_t v = sc_gscan(&t);

        if ((int32_t)v - (int32_t)prev <= SC_WRAP_DROP) { return t; }
        prev = v;
        if ((sc_cyc() - t0) > lim) { return 0; }
    }
}

/* ==================== gather：staging(行主序) → 板(列主序) ==================== */

/* 板 k = 逻辑列 [16k, 16k+15] x 全部 240 行。源 240 行 x 每行 32B 连续读，
 * PSRAM 读带宽板证 41MB/s（线性）→ 一板 ~0.2-0.4ms，藏在 DMA 的 819µs 里 */
static void sc_gather(uint32_t k)
{
    const uint16_t *src = s_stage + k * SC_SLAB_W;
    uint16_t *dst = s_slab[k & 1u];
    uint32_t h = bsp_lcd_height();

    for (uint32_t v = 0; v < h; v++)
    {
        for (uint32_t j = 0; j < SC_SLAB_W; j++)
        {
            dst[j] = src[j];
        }
        src += s_stride;
        dst += SC_SLAB_W;
    }
}

/* ==================== 对外接口 ==================== */

int bsp_lcd_scan_init(void)
{
    uint32_t tw[5];
    uint8_t  ok = 1;
    uint8_t  i;

    if (s_ready != 0) { return 0; }            /* 幂等 */

    if (bsp_psram_init() != 0) { return 1; }   /* staging 所在的 PSRAM 不可用 */

    if ((bsp_lcd_width() != 320u) || (bsp_lcd_height() != 240u))
    {
        return 2;   /* 标定坐标系是横屏 320x240；竖屏待另做标定再放开 */
    }

    /* T_scan 实测：4 个回绕间隔平均（板证漂移 0.0%、抖动 0.14%） */
    for (i = 0; i < 5u; i++)
    {
        tw[i] = sc_wait_wrap(50);
        if (tw[i] == 0) { ok = 0; break; }
    }
    if (ok == 0) { return 3; }                 /* GSCAN 无应答 */

    s_tscan_us = 0;
    for (i = 0; i < 4u; i++)
    {
        s_tscan_us += sc_ticks_to_us(tw[i + 1u] - tw[i]);
    }
    s_tscan_us /= 4u;
    if ((s_tscan_us < 8000u) || (s_tscan_us > 40000u)) { return 4; }

    s_stride  = bsp_lcd_width();
    s_nslab   = s_stride / SC_SLAB_W;
    s_pace_us = (s_tscan_us / s_nslab) + 8u;

    s_stage = (uint16_t *)(BSP_PSRAM_WINDOW_BASE + SC_STAGE_OFF);
    memset(s_stage, 0, 320u * 240u * 2u);      /* 一次性 ~3.7ms（PSRAM 写 42MB/s） */

    s_ready = 1;
    PSG_LOG("scan: 就绪 T_scan=%luus pace=%luus d0=%uus stage=0x%08lx\r\n",
            (unsigned long)s_tscan_us, (unsigned long)s_pace_us,
            (unsigned)SC_D0_US, (unsigned long)(BSP_PSRAM_WINDOW_BASE + SC_STAGE_OFF));
    return 0;
}

uint8_t bsp_lcd_scan_ready(void)
{
    return s_ready;
}

int bsp_lcd_scan_band_put(uint16_t x0, uint16_t y0,
                          uint16_t x1, uint16_t y1,
                          const uint16_t *px)
{
    uint32_t w = (uint32_t)x1 - x0 + 1u;
    uint32_t y;

    if (s_ready == 0) { return -1; }

    for (y = y0; y <= y1; y++)
    {
        uint16_t *dst = s_stage + y * s_stride + x0;

        for (uint32_t i = 0; i < w; i++)
        {
            dst[i] = px[i];
        }
        px += w;
    }
    return 0;
}

int32_t bsp_lcd_scan_frame_send(void)
{
    uint32_t t_wrap;
    uint32_t t0;
    uint32_t gather_ticks = 0;
    uint32_t us;
    uint32_t k;

    if (s_ready == 0) { return -3; }

    t_wrap = sc_wait_wrap(50);
    if (t_wrap == 0) { s_wrap_to++; return -1; }

    /* δ0：落后扫描一整块板的渡越 + 相位偏移 Δ（标定值，见文件头） */
    while ((sc_cyc() - t_wrap) < sc_us_to_ticks(SC_D0_US)) { }

    t0 = sc_cyc();

    /* 首板先 gather（此刻无 DMA 可并行）；之后 gather k+1 与 DMA k 流水 */
    {
        uint32_t tg = sc_cyc();

        sc_gather(0);
        gather_ticks += sc_cyc() - tg;
    }

    for (k = 0; k < s_nslab; k++)
    {
        uint32_t done0;
        uint32_t guard = SC_DMA_GUARD;
        uint32_t done;

        while ((sc_cyc() - t0) < sc_us_to_ticks(s_pace_us * k)) { }

        bsp_lcd_dma_stats(0, &done0, 0);
        bsp_lcd_set_window((uint16_t)(k * SC_SLAB_W), 0,
                           (uint16_t)(k * SC_SLAB_W + SC_SLAB_W - 1u),
                           bsp_lcd_height() - 1u);
        (void)bsp_lcd_write_pixels_async(s_slab[k & 1u],
                                         SC_SLAB_W * bsp_lcd_height());

        if (k + 1u < s_nslab)
        {
            uint32_t tg = sc_cyc();

            sc_gather(k + 1u);                 /* 藏进 DMA k 的 819µs 里 */
            gather_ticks += sc_cyc() - tg;
        }

        for (;;)                               /* 等 DMA k 完成 */
        {
            bsp_lcd_dma_stats(0, &done, 0);
            if (done > done0) { break; }
            if (--guard == 0)
            {
                bsp_lcd_dma_recover();
                s_dma_to++;
                return -2;                     /* 本帧中止，下一帧重试 */
            }
        }
    }

    us = sc_ticks_to_us(sc_cyc() - t0);
    s_frames++;
    s_sum_us += us;
    s_gather_sum += sc_ticks_to_us(gather_ticks);
    if (us > s_max_us) { s_max_us = us; }
    return (int32_t)us;
}

uint32_t bsp_lcd_scan_tscan_us(void)
{
    return s_tscan_us;
}

void bsp_lcd_scan_stats(uint32_t *frames, uint32_t *avg_us, uint32_t *max_us,
                        uint32_t *gather_avg_us, uint32_t *timeouts)
{
    if (frames        != 0) { *frames        = s_frames; }
    if (avg_us        != 0) { *avg_us        = (s_frames != 0)
                                     ? (uint32_t)(s_sum_us / s_frames) : 0u; }
    if (max_us        != 0) { *max_us        = s_max_us; }
    if (gather_avg_us != 0) { *gather_avg_us = (s_frames != 0)
                                     ? (uint32_t)(s_gather_sum / s_frames) : 0u; }
    if (timeouts      != 0) { *timeouts      = s_wrap_to + s_dma_to; }
}
