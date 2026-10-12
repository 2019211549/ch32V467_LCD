/*********************************************************************
 * test_lcd_perf.c - LCD 性能基线/阶梯测试（阶段 0）
 *
 * 原则：本文件只测量，不改任何驱动行为。
 *   - SPI 写时钟、轮询发送方式、初始化序列全部与点屏版一致
 *   - 计时用 SysTick 自由计数（HCLK/8=25MHz）——测试跑在调度器
 *     启动前，SysTick 尚未被 FreeRTOS 占用，测完调度器会重新配置它
 *
 * 图案设计（暴露不同错误类型）：
 *   solid    全屏纯色轮播 —— 基础带宽，暴露大面积花屏/断流
 *   bars     8 色条       —— 几何+颜色，暴露窗口切换错误
 *   checker  16px 棋盘    —— 300 个小窗口，暴露窗口命令开销 + 细粒度错位
 *   gray     30 级灰阶    —— 渐变，暴露位错位/字节序错误
 *
 * 输出：每图案 n 帧 / 平均 / 最小 / 最大 / 等效 FPS，与理论值对照。
 *********************************************************************/
#include "psg_config.h"
#include "test_config.h"

#if PSG_TEST_LCD_PERF

#include "ch32v4x7.h"       /* SysTick */
#include "test_framework.h"
#include "bsp_lcd.h"

/* 每个图案连续跑多少帧（阶段 1 提速 soak 时可加大） */
#define PERF_LOOPS        10   //每一个图案连续刷新几次

/* 帧间驻留（毫秒）：每帧刷完后停留再刷下一帧，供人眼看清瑕疵。
 * 高速时钟下（50MHz ≈ 25ms/帧）10 帧闪过根本看不清；缺陷已写进 GRAM
 * 会留在屏上，驻留只是让眼睛看清已写入的结果，不会掩盖高速问题。
 * 驻留发生在计时窗口之外，不影响 avg/min/max 统计。纯速度 soak 时设 0 */
#define PERF_FRAME_HOLD_MS   0

/* 当前写屏 SCK（与 bsp_lcd.c 的 LCD_SPI_WRITE_PRESCALER 保持同步，
 * 写速度改了这里不改的话理论值就是错的） */
#define PERF_SCK_HZ       100000000UL

/* 全屏纯像素负载理论耗时（µs）：153600B x 8 / SCK，编译期算好打印。
 * 中间乘法必须 64 位：RV32 上 unsigned long 只有 32bit，153600*8*1000000 会溢出 */
#define PERF_THEORY_US    ((uint32_t)(153600UL * 8UL * 1000000ULL / PERF_SCK_HZ))

static uint16_t pw, ph;     /* 逻辑宽高（横屏 320x240） */
static uint32_t color_idx;

typedef struct
{
    uint32_t n;
    uint32_t tmin;
    uint32_t tmax;
    uint64_t tsum;
} perf_stat_t;

static void stat_begin(perf_stat_t *s)
{
    s->n = 0;
    s->tmin = 0xFFFFFFFFu;
    s->tmax = 0;
    s->tsum = 0;
}

static void stat_add(perf_stat_t *s, uint32_t us)
{
    if (us < s->tmin) { s->tmin = us; }
    if (us > s->tmax) { s->tmax = us; }
    s->tsum += us;
    s->n++;
}

static void stat_report(const char *name, perf_stat_t *s)
{
    uint32_t avg_us = (uint32_t)(s->tsum / s->n);
    uint32_t fps10  = (avg_us == 0) ? 9999u : (uint32_t)(10000000UL / avg_us);

    /* ms 保留 1 位小数：阶段对比需要亚毫秒分辨率（uart_printf 不支持 %- 左对齐，用 %8s 右对齐） */
    uart_printf("[PERF] %8s n=%lu avg=%lu.%lums min=%lu.%lums max=%lu.%lums fps=%lu.%lu\r\n",
                name,
                (unsigned long)s->n,
                (unsigned long)(avg_us / 1000), (unsigned long)((avg_us % 1000) / 100),
                (unsigned long)(s->tmin / 1000), (unsigned long)((s->tmin % 1000) / 100),
                (unsigned long)(s->tmax / 1000), (unsigned long)((s->tmax % 1000) / 100),
                (unsigned long)(fps10 / 10), (unsigned long)(fps10 % 10));
}

/* ---------- SysTick 自由计数（HCLK/8 = 25MHz，1µs = 25 tick） ---------- */
static void perf_timer_init(void)
{
    SysTick->CTLR = 0;                     /* 停表 */
    SysTick->CNT  = 0;
    SysTick->CMP  = 0xFFFFFFFFu;           /* 不比较（不触发 SR/中断） */
    SysTick->SR   = 0;
    SysTick->CTLR = 0x1;                   /* STE=1, STCLK=0 -> HCLK/8 自由递增 */
}

static uint32_t perf_us(void)
{
    return SysTick->CNT / 25;
}

/* 帧间驻留：busy 等 PERF_FRAME_HOLD_MS（调度器未启动，没有 vTaskDelay 可用）。
 * 在 run_pattern 的计时窗口之外调用，统计值不受影响 */
static void perf_hold(void)
{
#if PERF_FRAME_HOLD_MS > 0
    uint32_t t0 = perf_us();

    while ((perf_us() - t0) < (uint32_t)PERF_FRAME_HOLD_MS * 1000UL)
    {
    }
#endif
}

/* ---------- 图案 ---------- */
static void pattern_solid(void)
{
    static const uint16_t colors[8] = {
        LCD_RED, LCD_GREEN, LCD_BLUE, LCD_WHITE,
        LCD_CYAN, LCD_MAGENTA, LCD_YELLOW, LCD_BLACK,
    };

    bsp_lcd_fill(0, 0, pw - 1, ph - 1, colors[color_idx++ & 7]);
}

static void pattern_bars(void)
{
    static const uint16_t colors[8] = {
        LCD_RED, LCD_GREEN, LCD_BLUE, LCD_WHITE,
        LCD_CYAN, LCD_MAGENTA, LCD_YELLOW, LCD_BLACK,
    };
    uint32_t i;

    for (i = 0; i < 8; i++)
    {
        uint16_t x0 = (uint16_t)(i * pw / 8);
        uint16_t x1 = (uint16_t)((i + 1) * pw / 8 - 1);

        bsp_lcd_fill(x0, 0, x1, ph - 1, colors[i]);
    }
}

static void pattern_checker(void)          /* 16x16 棋盘：300 个小窗口 */
{
    uint16_t bx, by;

    for (by = 0; by < ph; by += 16)
    {
        for (bx = 0; bx < pw; bx += 16)
        {
            uint16_t x1 = (uint16_t)((bx + 16 > pw) ? pw - 1 : bx + 15);
            uint16_t y1 = (uint16_t)((by + 16 > ph) ? ph - 1 : by + 15);

            bsp_lcd_fill(bx, by, x1, y1,
                         (((bx / 16) + (by / 16)) & 1) ? LCD_WHITE : LCD_BLACK);
        }
    }
}

static void pattern_gray(void)             /* 30 级垂直灰阶渐变 */
{
    uint32_t b;

    for (b = 0; b < 30; b++)
    {
        uint16_t x0 = (uint16_t)(b * pw / 30);
        uint16_t x1 = (uint16_t)((b + 1) * pw / 30 - 1);
        uint8_t  g  = (uint8_t)(b * 8);    /* 0..232 */

        bsp_lcd_fill(x0, 0, x1, ph - 1, LCD_COLOR(g, g, g));
    }
}

/* 跑一个图案 PERF_LOOPS 帧并统计 */
static void run_pattern(const char *name, void (*fn)(void))
{
    perf_stat_t st; //创建当前图案的一份统计信息
    uint32_t i; 
    uint32_t total0 = perf_us(); //t0是单独测试一帧的时间

    stat_begin(&st); //初始化结构 还没测试任何帧
    for (i = 0; i < PERF_LOOPS; i++)
    {
        uint32_t t0 = perf_us();

        fn();
        stat_add(&st, perf_us() - t0); //塞进统计器
        perf_hold();                   //驻留观屏（计时窗口外，不影响统计）
    }
    stat_report(name, &st);
    uart_printf("       (pattern total %lums, frames %lu)\r\n",
                (unsigned long)((perf_us() - total0) / 1000),
                (unsigned long)PERF_LOOPS);
}

void test_lcd_perf(void)
{
    uint32_t t_all0; //统计统计整个的时间

    /* MHz 带两位小数自动跟随 PERF_SCK_HZ，横幅不会因改速后忘改而失真 */
    uart_printf("\r\n[PERF] ===== PERF test: SCK=%lu.%02luMHz polling LANDSCAPE 320x240 =====\r\n",
                (unsigned long)(PERF_SCK_HZ / 10000UL / 100UL),
                (unsigned long)((PERF_SCK_HZ / 10000UL) % 100UL));
    uart_printf("[PERF] theory full-frame payload = %lu.%lums (153600B @SCK)\r\n",
                (unsigned long)(PERF_THEORY_US / 1000),
                (unsigned long)((PERF_THEORY_US % 1000) / 100));

    bsp_lcd_init();  //初始化lcd屏幕

    bsp_lcd_set_rotation(LCD_ROT_LANDSCAPE);
    pw = bsp_lcd_width();
    ph = bsp_lcd_height();
    color_idx = 0;

    perf_timer_init();  //初始化性能定时器
    t_all0 = perf_us();

    run_pattern("solid", pattern_solid);
    run_pattern("bars", pattern_bars);
    run_pattern("checker", pattern_checker);
    run_pattern("gray", pattern_gray);

    uart_printf("[PERF] all patterns done, total %lums, no hang.\r\n",
                (unsigned long)((perf_us() - t_all0) / 1000));
    uart_printf("[PERF] 请对照检查屏幕: 色条/棋盘/灰阶有无错位、断线、异色。\r\n");

    /* 跑完不挂死 = SPI 未卡死；像素正确性靠人眼按上面清单核 */
    TEST_ASSERT(1);
    
    while(1)
    {
        
    }
}

#endif /* PSG_TEST_LCD_PERF */
