/*********************************************************************
 * bsp_touch.c - FT6336U 电容触摸驱动（I3C 外设 legacy I2C 模式）
 *
 * 传输层为什么是 I3C：接线 PB4/PB5 恰好是 CH32V407 I3C 的重映射档 1
 * （I3C_SCL_1 / I3C_SDA_1，默认档 0 在 PB10/PB11，手册引脚定义表）。
 * I3C 控制器原生支持 legacy I2C 帧（CTLR.MTYPE=0100，RM 19.3.3.5），
 * 总线上看就是一条普通 I2C，FT6336 完全无感知。相比软件位拍：
 * 时序由外设发生，不受中断抢占影响，CPU 只管填/取 FIFO。
 *
 * 速率硬约束（调参前必读）：
 *   TIMINGR0 四个时段字段都是 8 位，单位 (N+1) × tHCLK = (N+1)×5ns
 *   （HCLK=200MHz，RM 19.4.16）。全取 0xFF 时 SCL 半周期 1.28us，
 *   即【本外设在此时钟下最慢只能跑 ~390kHz】，回不到 100kHz。
 *   FT6336 规格 10~400kHz，390kHz 贴着上限：上升沿质量全靠上拉。
 *   外设内部上拉已开（I3C_Ctrl_Config 的 PullUp），若上板 NACK /
 *   丢数据 → SDA/SCL 各加一颗 2.2k~4.7k 外部上拉到 3.3V。
 *
 * 帧结构（NOARBH=1：legacy 帧前不带 7E 仲裁头，纯 I2C 帧，RM 19.3.3.5）：
 *   读寄存器 = 两段独立 STOP 事务：S 0x70 reg P  +  S 0x71 data×N P
 *     （FT6x36 寄存器指针跨 STOP 保持，写完 reg 直接读即可，Arduino
 *      FT6206/6236 驱动同款写法）
 *   【板证 2026-10-13】不能用 RESTART 链拼复合读：写段以 RESTART 收尾后
 *     帧处于 ACTIVE 态，此时第二次写 CTLR（读段）不生效 —— 总线静默、
 *     RXFNEF/ERRF/FCF 全无（RM 流程图 "write (first) CTLR" + 本工程
 *     Control FIFO=DISABLE）。另：EVT 的 ENTDAA 逐字节轮询 RXFNEF 正常，
 *     证明 RXFNEF 不是阈值门控（同为 RXTHRES=1/4 配置）。
 *   地址字段 ADD[6:0] 填 7bit 的 0x38（读写方向 RNW 是独立位，不进地址）。
 *
 * 复位时序（FT6336U 手册 Table 3-5）：RST 低 >=5ms -> 高 ->
 * 等 >=300ms 才能开始通信。touch_delay_ms 与 bsp_lcd.c 的
 * lcd_delay_ms 同款（刻意偏慢，只多不少；各文件 static，不建公共文件）
 *********************************************************************/
#include "bsp_touch.h"
#include "ch32v4x7.h"
#include "ch32v4x7_i3c.h"

/* ==================== 引脚映射（与 bsp_lcd.h 风格一致） ==================== */
#define TOUCH_INT_PORT     GPIOA
#define TOUCH_INT_PIN      GPIO_Pin_8      /* PA8 <- CTP_INT（T1 只配置不使用） */
#define TOUCH_RST_PORT     GPIOC
#define TOUCH_RST_PIN      GPIO_Pin_4      /* PC4  -> CTP_RST 低有效 */

#define TOUCH_RST_LOW()    (TOUCH_RST_PORT->BCR  = TOUCH_RST_PIN)
#define TOUCH_RST_HIGH()   (TOUCH_RST_PORT->BSHR = TOUCH_RST_PIN)

/* SCL/SDA 是 PB4/PB5，由 GPIO_Remap_I3C 重映射 + AF_PP 交给 I3C 外设，
 * 不在本文件单独定义宏（没有软件位拍原语了） */

/* I3C 时序档：全 0xFF = 本外设最慢档，SCL ≈ 390kHz（见文件头硬约束） */
#define TOUCH_I3C_HALF     0xFFU

/* 帧超时：一笔 7 字节复合读 @390kHz 约 0.2ms，这里给 30ms 量级余量
 * （只慢不快；超时只在总线异常时才会走满，正常路径几十微秒就返回） */
#define TOUCH_I3C_TIMEOUT  300000U

/* 粗略毫秒延时（调度器启动前也要能用，见文件头） */
static void touch_delay_ms(uint32_t ms)
{
    volatile uint32_t n = ms * 60000UL;    /* 同 lcd_delay_ms 标定 */

    while (n--)
    {
    }
}

/* ==================== 失败现场记录（bring-up 诊断用） ====================
 * stage 码：上一笔失败事务死在哪一步（0 = 没失败过）
 *   1 = 写段 TXFNFF 超时（TX FIFO 满/外设无响应）
 *   2 = 写段 FCF 超时（帧启动后没完成；CTLR 是否已清零可区分"没启动"）
 *   3 = 写段 ERRF（地址被 NACK 等，看 STATER：0x100=地址 NACK）
 *   4 = 读段收数超时（总线长时间无数据无错误）
 *   5 = 读段 ERRF
 *   6 = 读段收完数但 FCF 超时
 * 配套快照：EVR（事件标志）、STATER（错误类型）、PCFR2（remap 是否置位）、
 * CTLR（消息字：若硬件取走会自行清零，非 0 = 消息没被受理）、
 * STATR（XDCNT = 实际已搬运字节数）
 * 由 bsp_touch_dbg_info() 读出、测试打印 —— 一次上板就能定位方向 */
static volatile uint32_t s_dbg_stage;    /* 0=无失败 */
static volatile uint32_t s_dbg_evr;
static volatile uint32_t s_dbg_stater;
static volatile uint32_t s_dbg_ctlr;     /* 失败时刻 CTLR：看消息字是否被吃掉 */
static volatile uint32_t s_dbg_statr;    /* 失败时刻 STATR：XDCNT 实际搬运数 */

static void touch_fail(uint32_t stage)
{
    s_dbg_stage  = stage;
    s_dbg_evr    = I3C->EVR;
    s_dbg_stater = I3C->STATER;
    s_dbg_ctlr   = I3C->CTLR;
    s_dbg_statr  = I3C->STATR;
}

void bsp_touch_dbg_info(uint32_t *stage, uint32_t *evr, uint32_t *stater,
                        uint32_t *pcfr2, uint32_t *ctlr, uint32_t *statr)
{
    if (stage  != 0) { *stage  = s_dbg_stage;  }
    if (evr    != 0) { *evr    = s_dbg_evr;    }
    if (stater != 0) { *stater = s_dbg_stater; }
    if (pcfr2  != 0) { *pcfr2  = AFIO->PCFR2;  }
    if (ctlr   != 0) { *ctlr   = s_dbg_ctlr;   }
    if (statr  != 0) { *statr  = s_dbg_statr;  }
}

/* ==================== I3C 事务原语 ==================== */
/* 有界等某个标志置位：0=等到，-1=超时 */
static int touch_i3c_wait_flag(uint32_t flag)
{
    volatile uint32_t n = TOUCH_I3C_TIMEOUT;

    while (n--)
    {
        if (I3C_GetFlagStatus(flag) != RESET) //一直在等待未满
        {
            return 0;
        }
    }
    return -1;
}

/* 总线出错后的统一收场：清标志 + 清 FIFO，让下一笔事务从干净状态开始 */
static void touch_i3c_abort(void)
{
    I3C_ClearFlag(I3C_FLAG_FCF | I3C_FLAG_ERRF);
    I3C_FlushAllFifo();
}

/* 等 FCF（帧完成）；期间 ERRF 置位算失败。返回 0 成功 / -1 失败已收场 */
static int touch_i3c_wait_frame(void)
{
    volatile uint32_t n = TOUCH_I3C_TIMEOUT;

    while (n--)
    {
        if (I3C_GetFlagStatus(I3C_FLAG_ERRF) != RESET)
        {
            touch_fail(3);
            touch_i3c_abort();
            return -1;
        }
        if (I3C_GetFlagStatus(I3C_FLAG_FCF) != RESET)
        {
            I3C_ClearFlag(I3C_FLAG_FCF);
            return 0;
        }
    }
    touch_fail(2);
    touch_i3c_abort();
    return -1;
}

/* 发一段 legacy I2C 写帧：S + 0x70(W) + data[0..len-1] + STOP。
 * 顺序与 EVT 例程一致：先 HandleMessage 启动帧，再往 TX FIFO 填字节
 * （地址相 9 个 SCL ≈23us，够 CPU 填数；反向"先填后启"在 WCH 上
 * 无例程佐证，T1 板证 preload 版字节不被取走，弃用） */
static int touch_legacy_write_stop(const uint8_t *data, uint8_t len)
{
    uint8_t i;

    I3C_ClearFlag(I3C_FLAG_FCF | I3C_FLAG_ERRF);

    I3C_ControllerHandleMessage(FT6336_I2C_ADDR, len, I3C_Direction_WR,
                                I3C_CONTROLLER_MTYPE_LEGACY_I2C,
                                I3C_GENERATE_STOP);

    for (i = 0; i < len; i++)
    {
        if (touch_i3c_wait_flag(I3C_FLAG_TXFNFF) != 0) //检查发送 FIFO 未满标志
        {
            touch_fail(1);
            touch_i3c_abort();
            return -1;
        }
        I3C_WriteByte(data[i]);
    }

    return touch_i3c_wait_frame();
}

/* 发一段 legacy I2C 读帧：S + 0x71(R) + N 字节 + P（末字节硬件回 NACK）
 * 收数双路判定：RXFNEF（FIFO 有货）为主；RXTGTENDF（读满 DCNT 目标）
 * 兜底 —— 一旦置位说明整条消息的字节都已进 FIFO，即使没到 FIFO 阈值
 * 也可以直接把剩余字节从 RDBR 读走 */
static int touch_legacy_read_stop(uint8_t *buf, uint8_t len)
{
    uint8_t got = 0;
    volatile uint32_t n = TOUCH_I3C_TIMEOUT;

    I3C_ClearFlag(I3C_FLAG_FCF | I3C_FLAG_ERRF | I3C_FLAG_RXTGTENDF);

    I3C_ControllerHandleMessage(FT6336_I2C_ADDR, len, I3C_Direction_RD,
                                I3C_CONTROLLER_MTYPE_LEGACY_I2C,
                                I3C_GENERATE_STOP);

    while (got < len)
    {
        if (I3C_GetFlagStatus(I3C_FLAG_RXFNEF) != RESET)
        {
            buf[got++] = I3C_ReadByte();
            n = TOUCH_I3C_TIMEOUT;
        }
        else if (I3C_GetFlagStatus(I3C_FLAG_RXTGTENDF) != RESET)
        {
            while (got < len)                    /* 剩余字节全在 FIFO 里 */
            {
                buf[got++] = I3C_ReadByte();
            }
        }
        else if (I3C_GetFlagStatus(I3C_FLAG_ERRF) != RESET)
        {
            touch_fail(5);
            touch_i3c_abort();
            return -1;
        }
        else if (n-- == 0U)
        {
            touch_fail(4);
            touch_i3c_abort();
            return -1;
        }
    }

    if (touch_i3c_wait_flag(I3C_FLAG_FCF) != 0) //FCF 用于判断通信帧完成
    {
        touch_fail(6);
        touch_i3c_abort();
        return -1;
    }
    I3C_ClearFlag(I3C_FLAG_FCF | I3C_FLAG_RXTGTENDF);
    return 0;
}

/* ==================== 对外 API ==================== */
int bsp_touch_read_regs(uint8_t reg, uint8_t *buf, uint8_t len)  //寄存器地址 接受数据的缓存地址  多大长度
{
    if ((buf == 0) || (len == 0))
    {
        return -1;
    }

    /* 两段独立 STOP 事务（文件头【板证 2026-10-13】：RESTART 链的第二段
     * CTLR 写不生效，不能用复合读）：
     *   S 0x70 reg P   —— 写寄存器号，指针落在 FT6336 内部
     *   S 0x71 data×N P —— 从当前指针连续读 */
    {
        uint8_t r = reg;

        if (touch_legacy_write_stop(&r, 1) != 0)
        {
            return -1;
        }
    }
    return touch_legacy_read_stop(buf, len);
}

int bsp_touch_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t b[2];

    b[0] = reg;
    b[1] = val;
    return touch_legacy_write_stop(b, 2);
}

int bsp_touch_read(uint16_t *x, uint16_t *y, uint8_t *pressed)  //最后一个参数是是否检测到了触摸
{
    uint8_t raw[5];    /* 0x02~0x06: TD_STATUS + P1_XH/XL/YH/YL */

    if ((x == 0) || (y == 0) || (pressed == 0))
    {
        return -1;
    }
    *x = 0;
    *y = 0;
    *pressed = 0;

    if (bsp_touch_read_regs(FT6336_REG_TD_STATUS, raw, sizeof(raw)) != 0)
    {
        return -1;
    }

    /* 触点数低 4 位；0=无触摸，>2 = 数据异常按无触摸处理（文档 §4.2） */
    {
        uint8_t cnt = raw[0] & 0x0FU;  //解析是否有触摸

        if ((cnt >= 1U) && (cnt <= 2U))
        {
            *x = (uint16_t)(((raw[1] & 0x0FU) << 8) | raw[2]);
            *y = (uint16_t)(((raw[3] & 0x0FU) << 8) | raw[4]);
            *pressed = 1;
        }
    }
    return 0;
}

int bsp_touch_init(void)
{
    static uint8_t s_alive;    /* 幂等：成功过就直接返回 —— 测试用例和
                                * task_ui 的 indev 都会调 init，二次调会
                                * 再走 400ms 复位时序并把在跑的读打断 */
    GPIO_InitTypeDef     g = {0};
    I3C_Ctrl_BusTypeDef  bus = {0};
    I3C_CtrlConfTypeDef  ctrl = {0};
    I3C_FifoConfTypeDef  fifo = {0};
    uint8_t id = 0;

    if (s_alive != 0)
    {
        return 0;
    }

    /* 时钟：GPIOA/B/C + AFIO（重映射要它）+ I3C（挂 HB 总线） */
    RCC_PB2PeriphClockCmd(RCC_PB2Periph_GPIOA | RCC_PB2Periph_GPIOB |
                          RCC_PB2Periph_GPIOC | RCC_PB2Periph_AFIO, ENABLE);
    RCC_HBPeriphClockCmd(RCC_HBPeriph_I3C, ENABLE);
    I3C_DeInit();                          /* 复位外设到出厂状态再配 */

    /* PC4 RST：推挽输出，先拉低进复位 */
    TOUCH_RST_LOW();
    g.GPIO_Pin   = TOUCH_RST_PIN;
    g.GPIO_Speed = GPIO_Speed_High;
    g.GPIO_Mode  = GPIO_Mode_Out_PP;
    GPIO_Init(TOUCH_RST_PORT, &g);

    /* PA8 INT：输入上拉，T1 不读它 */
    g.GPIO_Pin  = TOUCH_INT_PIN;
    g.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_Init(TOUCH_INT_PORT, &g);

    /* PB4/PB5：重映射档 1 -> I3C_SCL/I3C_SDA，复用推挽交给外设驱动 */
    GPIO_PinRemapConfig(GPIO_Remap_I3C, ENABLE);
    g.GPIO_Pin   = GPIO_Pin_4 | GPIO_Pin_5;
    g.GPIO_Speed = GPIO_Speed_High;
    g.GPIO_Mode  = GPIO_Mode_AF_PP;
    GPIO_Init(GPIOB, &g);

    /* 复位时序（手册 Table 3-5）：低 >=5ms -> 高 -> 等 >=300ms */
    touch_delay_ms(10);
    TOUCH_RST_HIGH();
    touch_delay_ms(350);

    /* 控制器时序：legacy I2C 只用 OD 低电平段 + I2C 高电平段两个字段，
     * 全 0xFF = 最慢档 SCL≈390kHz（文件头硬约束）；另两个 PP/I3C 档
     * 字段本场景用不到，一并填满保持一致 */
    bus.SDAHoldTime        = I3C_SDAHoldTime_1_5;
    bus.WaitTime           = I3C_WaitTime_State_2;
    bus.SCLPPLowDuration   = TOUCH_I3C_HALF;   /* I3C 推挽档（legacy 用不到） */
    bus.SCLI3CHighDuration = TOUCH_I3C_HALF;   /* I3C 档（legacy 用不到） */
    bus.SCLODLowDuration   = TOUCH_I3C_HALF;   /* legacy I2C：SCL 低 1.28us */
    bus.SCLI2CHighDuration = TOUCH_I3C_HALF;   /* legacy I2C：SCL 高 1.28us */
    bus.BusFreeDuration    = TOUCH_I3C_HALF;   /* STOP->START 总线空闲间隔 */
    bus.BusIdleDuration    = TOUCH_I3C_HALF;
    I3C_Ctrl_Init(&bus);

    ctrl.DynamicAddr = 0x22;    /* 本控制器当从机时的动态地址（触摸场景用不到） */
    ctrl.StallTime   = 0x10;
    ctrl.PullUp      = ENABLE;  /* 外设内部上拉：legacy 高速档必须有像样的上拉 */
    I3C_Ctrl_Config(&ctrl);

    fifo.RxFifoThreshold = I3C_RXFIFO_THRESHOLD_1_4;
    fifo.TxFifoThreshold = I3C_TXFIFO_THRESHOLD_1_4;
    fifo.ControlFifo     = I3C_CONTROLFIFO_DISABLE;   /* 无 DMA，控制 FIFO 关闭 */
    I3C_SetConfigFifo(&fifo);

    I3C_ArbitrationHeaderCmd(ENABLE);  /* 置 NOARBH=1：legacy 帧不带 7E 仲裁头，
                                          纯 I2C 帧，FT6336 才认（RM 19.3.3.5） */
    I3C_FlushAllFifo();
    I3C_Cmd(ENABLE);

    /* 探测：完整走一遍复合读 FIRMID。能读回 = 重映射/时序/上拉/芯片全通。
     * 失败等 5ms 重试一次（复位后首帧偶发不响应），再失败现场已记入
     * s_dbg_*，由测试经 bsp_touch_dbg_info() 打出定位 */
    if (bsp_touch_read_regs(FT6336_REG_FIRMID, &id, 1) != 0)
    {
        touch_delay_ms(5);
        if (bsp_touch_read_regs(FT6336_REG_FIRMID, &id, 1) != 0) //重试
        {
            return -1;
        }
    }
    s_alive = 1;
    return 0;
}
