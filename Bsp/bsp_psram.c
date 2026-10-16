/*********************************************************************
 * bsp_psram.c - CH32V467 片上 PSRAM(4MB) 初始化 + 自检实现
 *
 * 序列出处（2026-10-15 取回，逐行对照搬移，勿按 RM 手写）：
 *   github.com/openwch/ch32v407_ch32v467
 *   EVT/EXAM/PSRAM/PSRAM/User/main.c   —— PSRAM_INIT() + PWR_VDD18LevelConfig
 *   EVT/EXAM/PSRAM/PSRAM/User/PSRAM.c  —— SetWrLatency/SetRdLatency/Wait_Busy_0
 *   EVT/EXAM/PSRAM/PSRAM/User/PSRAM.h  —— 延迟码表（vendored v1.4 库没有）
 *
 * 与 EVT 例程的差异（只有这三处，其余一致）：
 *   1. SYSCLK 300M/HCLK 150M（system_ch32v4x7.c 预设，与 EVT 例程同款；
 *      曾用 400M/200M，2026-10-16 板证 PSRAM 400M 档出局，见下梯子）
 *   2. Delay_Ms(1)/Delay_Us(20) 换成本文件软件延时（工程删了 Delay_Init：
 *      与 FreeRTOS tick 抢 SysTick）
 *   3. 末尾加了数据自检 + MR 回读打印 + die MR 复位（EVT 在 main 循环里
 *      做自检；die 复位是 EVT Global_RST_Set 的等价物——例程冷启动不需要，
 *      我们反复烧录暖复位需要清残留 MR）
 *
 * 失败回退梯子（自检失配时按序试，一次只动一个变量）：
 *   档1 400M行 + MR8(仅wr路) + trc=0x14 —— FAIL：trc 20周期@400M=50ns <
 *       die Trc 60ns，真实违例（自检第 3 个半字失配）
 *   档2 trc→0x1E（75ns）—— 仍 FAIL：16/40K 半字且全部=前一个字的值
 *       → 排除信号完整性/地址线，锁定读路径时序
 *   档2.5 rd 路 MR8 补写（EVT SetRdLatency 第三步）—— 仍 FAIL 且恶化到
 *       38/40K、出现跨事务陈旧读（别名点4读回点0值）。MR 回读证伪了
 *       "MR0 写入清 MR8"假设：低 8 位 04/80/20 全对 = die 配置无误，
 *       是 400M 档本身跑不动 → 定论：该 die/板 400M 不可用
 *   档3 SYSCLK 300M + 300M 行 + die MR 复位（2026-10-16 起，本版，=
 *       EVT 例程完全同款配置）
 *   档4 240M 预设 + 225M 行（MR4=0x5 MR0=0x5 Latency=0x8，EVT 表里 240M
 *       无行、向下取 225M 行保守覆盖）      档5 CFIFO→WRADD（0x10）
 *********************************************************************/
#include "ch32v4x7.h"
#include "ch32v4x7_psram.h"
#include "ch32v4x7_rcc.h"
#include "ch32v4x7_pwr.h"
#include "bsp_uart.h"      /* uart_printf：自检失配打印（PSG_LOG 走它） */
#include "psg_config.h"
#include "bsp_psram.h"

/* ==================== 延迟档选择（失败回退梯子的旋钮） ====================
 * 可选：MR4_Write_300M/MR0_Read_300M/Latency_300M（默认，与 SYSCLK 300M 配套）
 *       MR4_Write_400M/MR0_Read_400M/Latency_400M（已板证出局：MR 配置回读全对
 *       仍持续读回陈旧拍且随流量恶化 16→38，含跨事务旧值） */
#define BSP_PSRAM_WR_FREQ      MR4_Write_300M
#define BSP_PSRAM_RD_FREQ      MR0_Read_300M
#define BSP_PSRAM_LATENCY      Latency_300M

/* ==================== EVT PSRAM.h 的码表（原样搬移） ====================
 * CMD_MR_READ/CMD_MR_WRITE 是运行时从控制器 CMD1_CFG 里读默认命令字，
 * 不是常量 —— EVT 也这么读，别写死 */
#define CMD_MR_ADDR_0          ((uint32_t)0x00)
#define CMD_MR_ADDR_4          ((uint32_t)0x04)
#define CMD_MR_ADDR_8          ((uint32_t)0x08)
#define CMD_MR_READ            ((uint32_t)((PSRAM->CMD1_CFG) & 0xFF))
#define CMD_MR_WRITE           ((uint32_t)(((PSRAM->CMD1_CFG) >> 8) & 0xFF))
#define Hfreq_En               ((uint32_t)0x01)

/* MR4 写延迟码（按 PSRAM 时钟档选） */
#define MR4_Write_300M         ((uint32_t)0x7)
#define MR4_Write_400M         ((uint32_t)0x4)
/* MR0 读延迟码 */
#define MR0_Read_300M          ((uint32_t)0x7)
#define MR0_Read_400M          ((uint32_t)0x1)
/* 控制器侧访问延迟（周期数） */
#define Latency_300M           ((uint32_t)0xb)
#define Latency_400M           ((uint32_t)0x10)
/* die MR 全局复位命令字（EVT PSRAM.h：从 CMD1_CFG[23:16] 读默认值） */
#define CMD_MR_RESET           ((uint32_t)(((PSRAM->CMD1_CFG) >> 16) & 0xFF))
/* 读延迟类型：Variable（数据就绪即返回）——EVT 例程传的就是它；
 * Fixed 需第二次 MR0 写（EVT 例程自己也没跑过），留作回退梯子备选 */
#define Read_Variable          ((uint32_t)0x0)
#define Read_Fixed             ((uint32_t)0x1)
#define BSP_PSRAM_RD_TYPE      Read_Variable

/* ==================== EVT PSRAM.c 的流程（原样搬移） ==================== */
static void psram_wait_busy(void)
{
    while ((PSRAM->ADDR & CMD_BUSY) == 1)   /* wait busy clear */
    {
    }
}

/* MR4：die 写延迟 + 控制器写延迟；≥333M 档还要开 MR8 高频模式 */
static void psram_set_wr_latency(uint32_t freq_code, uint32_t latency)
{
    PSRAMSetData(freq_code << 5);
    PSRAMSetWrLatency(latency);
    PSRAM_Set_MR_ADDR(CMD_MR_ADDR_4);
    PSRAM_Set_CMD(CMD_MR_WRITE);
    PSRAM_Set_Busy(ENABLE);
    PSRAM_Set_MW(ENABLE);
    psram_wait_busy();

    if ((freq_code == MR4_Write_400M))      /* 400M 档：高频延迟使能（EVT 同款） */
    {
        PSRAMSetData(Hfreq_En << 5);
        PSRAM_Set_MR_ADDR(CMD_MR_ADDR_8);
        PSRAM_Set_CMD(CMD_MR_WRITE);
        PSRAM_Set_Busy(ENABLE);
        PSRAM_Set_MW(ENABLE);
        psram_wait_busy();
    }
}

/* MR 寄存器读回（EVT PSRAMReadReg 原样）：走 CMD_MR_READ 命令路径、
 * 不走数据口，数据口时序坏着也能用——2026-10-16 定位的关键观察口。
 * 注：die 的 MR 只有 8 位，回读高 8 位是读 FIFO 残留（上轮板证
 * MR0=9a04/MR4=2080/MR8=0420——低 8 位 04/80/20 全对），打印时掩掉 */
static uint32_t psram_read_reg(uint32_t mr_addr)
{
    PSRAM_Set_MR_ADDR(mr_addr);
    PSRAM_Set_CMD(CMD_MR_READ);
    PSRAM_Set_Busy(ENABLE);
    PSRAM_Set_MW(CMD_MW_Read);          /* EVT 同名常量(vendored 头=0)，非 ENABLE */
    psram_wait_busy();
    return PSRAM->DATA;
}

/* MR0：die 读延迟 + 控制器读延迟；400M 档写完 MR0 后还要【再写一次
 * MR8 Hfreq】（EVT SetRdLatency 第三步，2026-10-16 之前漏搬的根因）。
 * 读延迟类型经 BSP_PSRAM_RD_TYPE 选，Variable 时与 EVT 例程路径一致 */
static void psram_set_rd_latency(uint32_t freq_code, uint32_t latency)
{
    PSRAMSetData((freq_code << 2) | (BSP_PSRAM_RD_TYPE << 5));
    PSRAMSetRdLatency(latency);
    PSRAM_Set_MR_ADDR(CMD_MR_ADDR_0);
    PSRAM_Set_CMD(CMD_MR_WRITE);
    PSRAM_Set_Busy(ENABLE);
    PSRAM_Set_MW(ENABLE);
    psram_wait_busy();

    /* 观察口：MR0 写完读回（掩低 8 位——高 8 位是 FIFO 残留噪声）。
     * 300M 档期望 MR0=1c MR4=e0 MR8=00（die 复位后 300M 档不写 MR8） */
    PSG_LOG("bsp_psram: MR0写后回读 MR0=%02x MR4=%02x MR8=%02x (期望 %02x/%02x/%02x)\r\n",
            (unsigned)(psram_read_reg(CMD_MR_ADDR_0) & 0xFFu),
            (unsigned)(psram_read_reg(CMD_MR_ADDR_4) & 0xFFu),
            (unsigned)(psram_read_reg(CMD_MR_ADDR_8) & 0xFFu),
            (unsigned)(((BSP_PSRAM_RD_FREQ << 2) | (BSP_PSRAM_RD_TYPE << 5)) & 0xFFu),
            (unsigned)((BSP_PSRAM_WR_FREQ << 5) & 0xFFu),
            (unsigned)((BSP_PSRAM_WR_FREQ == MR4_Write_400M) ? (Hfreq_En << 5) : 0u));

    if (freq_code == MR0_Read_400M)     /* EVT: 333M/400M 档读路径同样要 MR8 */
    {
        PSRAMSetData(Hfreq_En << 5);
        PSRAM_Set_MR_ADDR(CMD_MR_ADDR_8);
        PSRAM_Set_CMD(CMD_MR_WRITE);
        PSRAM_Set_Busy(ENABLE);
        PSRAM_Set_MW(ENABLE);
        psram_wait_busy();
    }
}

/* 粗略毫秒延时（init 时序用，只多不少）：同 bsp_lcd.c lcd_delay_ms 的保守值 */
static void psram_delay_ms(uint32_t ms)
{
    volatile uint32_t n = ms * 60000UL;     /* HCLK 降频后单位时长只会更长，安全 */

    while (n--)
    {
    }
}

/* die MR 全局复位（EVT Global_RST_Set 原样 + 保守延时）：反复烧录/暖复位
 * 下 die 不掉电，MR 会残留上一次的值（板上现在就残留着 400M 档的
 * MR8=0x20）——300M 档不再写 MR8，不复位的话 die 状态 ≠ EVT 例程的
 * 上电默认态。复位后立刻重配全部 MR，每次都从默认态出发 */
static void psram_die_reset(void)
{
    PSRAM_Set_CMD(CMD_MR_RESET);
    PSRAM_Set_Busy(ENABLE);
    psram_delay_ms(1);      /* EVT: Delay_Us(20)（复位至少 2µs），保守 1ms+ */
}

/* 数据自检：窗口顶/底各写走步+重复图案再读回（避开 [0,帧缓冲区) 区间，
 * 顶部的 4KB 探测区在 LVGL 帧缓冲 memset 之前执行也不冲突——本函数
 * 先跑、且落在帧缓冲区之外）。
 * 返回 0 = 全对；非 0 = 第一个失配的字节偏移+1 */
static uint32_t psram_data_check(void)
{
    volatile uint16_t *probe =
        (volatile uint16_t *)(BSP_PSRAM_WINDOW_BASE + BSP_PSRAM_SIZE - 2048);
    uint16_t i;

    for (i = 0; i < 1024; i++)
    {
        probe[i] = (uint16_t)(0xA500 | i);      /* 低10位走步 + 高位固定 */
    }
    for (i = 0; i < 1024; i++)
    {
        if (probe[i] != (uint16_t)(0xA500 | i))
        {
            return (uint32_t)i + 1;             /* 失配：报探测区内偏移 */
        }
    }

    /* 地址相关性复查：跨 16KB 间隔写 8 个字（抓地址线粘连） */
    for (i = 0; i < 8; i++)
    {
        *(volatile uint32_t *)(BSP_PSRAM_WINDOW_BASE + (uint32_t)(i * 2) * 1024) =
            0xDEAD0000UL | (uint32_t)i;
    }
    for (i = 0; i < 8; i++)
    {
        if (*(volatile uint32_t *)(BSP_PSRAM_WINDOW_BASE + (uint32_t)(i * 2) * 1024)
                != (0xDEAD0000UL | (uint32_t)i))
        {
            return 0x10000UL + (uint32_t)i;     /* 高位标记：第二组失配 */
        }
    }
    return 0;
}

static uint8_t s_psram_state;    /* 0=未初始化 1=可用 2=自检失配 */

int bsp_psram_init(void)
{
    PSRAMInitTypeDef    init = {0};
    PSRAMTimingInitTypeDef timing = {0};
    uint32_t bad;

    if (s_psram_state != 0)      /* 幂等：返回首次结果 */
    {
        return (s_psram_state == 1) ? 0 : 1;
    }

    /* EVT main.c 顺序：先 VDD18 电平（Level1），再 PSRAM_INIT()。
     * EVT 没调 PWR_VDD18Cmd，照抄 */
    PWR_VDD18LevelConfig(PWR_VDD18_Level1);

    RCC_HBPeriphClockCmd(RCC_HBPeriph_PSRAM, ENABLE);
    PSRAMDeInit();

    /* 时序：EVT 原值 trc=0x14。300M 下 20 周期≈67ns ≥ die Trc 60ns 合规
     * （2026-10-16 板证：400M 下 50ns 真违例，但加大到 0x1E 也没救回——
     * 400M 档整体出局，问题不止 trc 一处） */
    timing.PSRAM_trc     = 0x14;
    timing.PSRAM_tcph    = 0x0C;
    timing.PSRAM_txlpd   = 0x07;
    init.PSRAM_cfifo     = PSRAM_CFIFO_BTWWRRD;
    init.PSRAM_cap_cfg   = PSRAM_CAPCFG_32M;      /* CH32V467RET = 4MB */
    init.PSRAM_exti_lpmd = PSRAM_EXIT_LPMD;       /* ch32v4x7.h:0x00010000 */
    init.PSRAMTimingStruct = &timing;
    PSRAMInit(&init);

    psram_delay_ms(1);            /* EVT: Delay_Ms(1) —— 等控制器/介质就绪 */

    psram_die_reset();            /* 清掉暖复位残留的旧 MR（如 400M 档的 MR8）*/
    psram_set_wr_latency(BSP_PSRAM_WR_FREQ, BSP_PSRAM_LATENCY);
    psram_set_rd_latency(BSP_PSRAM_RD_FREQ, BSP_PSRAM_LATENCY);

    /* 终态回读：三个 MR 全对 = die 配置正确（再失配就不是配置问题了） */
    PSG_LOG("bsp_psram: MR终态 MR0=%02x MR4=%02x MR8=%02x\r\n",
            (unsigned)(psram_read_reg(CMD_MR_ADDR_0) & 0xFFu),
            (unsigned)(psram_read_reg(CMD_MR_ADDR_4) & 0xFFu),
            (unsigned)(psram_read_reg(CMD_MR_ADDR_8) & 0xFFu));

    bad = psram_data_check();
    if (bad != 0)
    {
        s_psram_state = 2;
        PSG_LOG("bsp_psram: self-test FAIL @probe+%lu (wr=%x rd=%x)\r\n",
                (unsigned long)(bad & 0xFFFFUL),
                (unsigned)BSP_PSRAM_WR_FREQ, (unsigned)BSP_PSRAM_RD_FREQ);
        return 1;
    }

    s_psram_state = 1;
    PSG_LOG("bsp_psram: init OK (4MB @0x80000000, %s档)\r\n",
            (BSP_PSRAM_WR_FREQ == MR4_Write_400M) ? "400M" : "300M");
    return 0;
}

uint8_t bsp_psram_ready(void)
{
    return (s_psram_state == 1) ? 1 : 0;
}
