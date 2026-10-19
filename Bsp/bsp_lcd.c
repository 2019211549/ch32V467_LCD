/*********************************************************************
 * bsp_lcd.c - P032B017P (ST7789P3) SPI1 驱动实现
 *
 * 本驱动为纯轮询实现，不依赖 FreeRTOS —— 调度器启动前（单元测试）
 * 和任务里（将来 LVGL flush）都能用。
 *
 * 初始化序列说明：preliminary datasheet 没有给本模组验证过的完整
 * init 表（摘要 §13），这里用 ST7789 家族最小公倍序列（复位默认
 * 电源/gamma 直接出图）。若颜色/显示异常，先调 bsp_lcd.h 顶部
 * 注释里的三个调参点，仍不行则向供应商要正式 init 代码。
 *********************************************************************/
#include "ch32v4x7.h"
#include "bsp_uart.h"     /* uart_printf：loopback 诊断打印 */
#include "bsp_lcd.h"

/* ST7789P3 常用命令 */
#define CMD_SLPIN       0x10
#define CMD_SLPOUT      0x11
#define CMD_INVOFF      0x20
#define CMD_INVON       0x21
#define CMD_CASET       0x2A
#define CMD_RASET       0x2B
#define CMD_RAMWR       0x2C
#define CMD_MADCTL      0x36
#define CMD_COLMOD      0x3A
#define CMD_DISPON      0x29

/* 写屏 SPI 速度（唯一调频旋钮，改这一行即可整体切换）：
 * PCLK2=200MHz。已试档位：/32=6.25MHz（基线 PASS + 2026-10-12 LVGL 全链路
 * 目视 PASS）、/2=100MHz（硬件分频极限；GPIO 档位仅 50MHz 级属超规格使用，
 * 2026-10-12 渲染链路修通后恢复，借 LVGL Hello 做目视确认——若出现花屏/
撕裂/错位逐级降 /4(50M)、/8(25M) 定位信号完整性）。
 * 注意：改这里须同步 test_lcd_perf.c 的 PERF_SCK_HZ。
 * init / 自环测试 / 读回恢复 三处同源引用，保证写速度一致 */
#define LCD_SPI_WRITE_PRESCALER   SPI_BaudRatePrescaler_2

/* ---------- 10.1 底层原语寄存器化（性能优化第一步） ----------
 * GPIO 库函数（函数调用+参数检查，约 20~40 周期）换成 BSHR/BCR 单条
 * 存储指令（2~4 周期）；SPI 标志查询/数据写入直访 STATR/DATAR。
 * 写的是同一批寄存器，时序行为与库函数版本完全等价。 */
#define LCD_CS_LOW()    (LCD_CS_PORT->BCR  = LCD_CS_PIN)    /* 片选拉低 */
#define LCD_CS_HIGH()   (LCD_CS_PORT->BSHR = LCD_CS_PIN)    /* 片选释放 */
#define LCD_DC_LOW()    (LCD_DC_PORT->BCR  = LCD_DC_PIN)    /* DC=命令 */
#define LCD_DC_HIGH()   (LCD_DC_PORT->BSHR = LCD_DC_PIN)    /* DC=数据 */
#define LCD_RST_LOW()   (LCD_RST_PORT->BCR  = LCD_RST_PIN)  /* 复位拉低 */
#define LCD_RST_HIGH()  (LCD_RST_PORT->BSHR = LCD_RST_PIN)  /* 复位释放 */

/* §11 DMA 引擎（实现在文件后部）：前置声明供前面的对外 API 入口防御使用 */
static void lcd_dma_init(void);
static void lcd_dma_sync(void);

/*********************************************************************
 *                      SPI1 / GPIO 底层原语
 *********************************************************************/

/* 粗略毫秒延时：本驱动可能在调度器启动前使用（无 SysTick/vTaskDelay），
 * 用保守软件延时，只用于 init 时序（只多不少即可） */
static void lcd_delay_ms(uint32_t ms)
{
    volatile uint32_t n = ms * 60000UL;     /* HCLK=200MHz 下约 1.2~1.5ms/单位 */

    while (n--)
    {
    }
}

/* SPI1 参数配置（init 和读回降速都走这里）：
 * 主模式，Mode0（SCK 上升沿采样，符合 ST7789 要求），软 NSS */
static void spi1_reconfig(uint16_t prescaler)
{
    SPI_InitTypeDef s = {0};

    SPI_Cmd(SPI1, DISABLE);

    s.SPI_Direction         = SPI_Direction_2Lines_FullDuplex;
    s.SPI_Mode              = SPI_Mode_Master;
    s.SPI_DataSize          = SPI_DataSize_8b;
    s.SPI_CPOL              = SPI_CPOL_Low;
    s.SPI_CPHA              = SPI_CPHA_1Edge;
    s.SPI_NSS               = SPI_NSS_Soft;
    s.SPI_BaudRatePrescaler = prescaler;
    s.SPI_FirstBit          = SPI_FirstBit_MSB;
    s.SPI_CRCPolynomial     = 7;
    SPI_Init(SPI1, &s);
    SPI_Cmd(SPI1, ENABLE);
}

static void spi1_init(void)
{
    GPIO_InitTypeDef  g = {0};

    RCC_PB2PeriphClockCmd(RCC_PB2Periph_GPIOA | RCC_PB2Periph_GPIOC |
                          RCC_PB2Periph_SPI1, ENABLE);

    /* SCK / MOSI：复用推挽 */
    g.GPIO_Pin   = LCD_SCK_PIN | LCD_MOSI_PIN;
    g.GPIO_Speed = GPIO_Speed_High;
    g.GPIO_Mode  = GPIO_Mode_AF_PP;
    GPIO_Init(GPIOA, &g);

    /* MISO(SDO)：读回诊断用，浮空输入 */
    g.GPIO_Pin  = LCD_MISO_PIN;
    g.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(GPIOA, &g);

    /* CS / DC / RST：普通推挽输出 */
    g.GPIO_Pin   = LCD_CS_PIN;
    g.GPIO_Speed = GPIO_Speed_High;
    g.GPIO_Mode  = GPIO_Mode_Out_PP;
    GPIO_Init(LCD_CS_PORT, &g);

    g.GPIO_Pin = LCD_DC_PIN;
    GPIO_Init(LCD_DC_PORT, &g);

    g.GPIO_Pin = LCD_RST_PIN;
    GPIO_Init(LCD_RST_PORT, &g);

    LCD_CS_HIGH();                               /* CS 先释放 */
    LCD_RST_HIGH();                              /* RST 拉高 */

    /* PCLK2=200MHz；写速度由 LCD_SPI_WRITE_PRESCALER 决定（见文件头） */
    spi1_reconfig(LCD_SPI_WRITE_PRESCALER);
}

static void spi1_send_byte(uint8_t b)
{
    while ((SPI1->STATR & SPI_I2S_FLAG_TXE) == 0)    //等待发送tx就绪（直访STATR）
    {
    }
    SPI1->DATAR = b;
}

/* 等待移位寄存器真正发完 —— 切换 DC/CS 前必须调用，
 * 否则最后一两位还在线上，DC 电平变了会被屏当成命令 */
static void spi1_wait_idle(void)
{
    while ((SPI1->STATR & SPI_I2S_FLAG_TXE) == 0)    //数据寄存器空
    {
    }
    while ((SPI1->STATR & SPI_I2S_FLAG_BSY) != 0)    //移位寄存器空
    {
    }
}

/* 全双工收发一个字节（读回时用：MOSI 发哑元，取 MISO 收到的） */
static uint8_t spi1_xfer_byte(uint8_t b)
{
    while ((SPI1->STATR & SPI_I2S_FLAG_TXE) == 0)
    {
    }
    SPI1->DATAR = b;
    while ((SPI1->STATR & SPI_I2S_FLAG_RXNE) == 0)
    {
    }
    return (uint8_t)SPI1->DATAR;
}

static void lcd_write_cmd(uint8_t cmd)
{
    LCD_DC_LOW();                                   /* DC 低 = 命令 */
    LCD_CS_LOW();
    spi1_send_byte(cmd);
    spi1_wait_idle();
    LCD_CS_HIGH();
}

static void lcd_write_data(const uint8_t *d, uint32_t len)
{
    uint32_t i;

    LCD_DC_HIGH();                                  /* DC 高 = 数据 */
    LCD_CS_LOW();
    for (i = 0; i < len; i++)
    {
        spi1_send_byte(d[i]);
    }
    spi1_wait_idle();
    LCD_CS_HIGH();
}

static void lcd_write_data1(uint8_t v)
{
    lcd_write_data(&v, 1);
}

/*********************************************************************
 *                          对外 API
 *********************************************************************/

/* 当前方向（LCD_ROT_xxx），init 后默认竖屏 */
static uint8_t lcd_rotation = LCD_ROT_PORTRAIT;

/* 各方向的 MADCTL 值：
 * bit7 MY 上下翻 | bit6 MX 左右翻 | bit5 MV 行列交换(横竖切换)
 * 红蓝颠倒在表里每个值 |0x08 */
static const uint8_t rot_madctl[4] = {
    0x00,       /* 0°   竖屏 */
    0x60,       /* 90°  横屏 */
    0xC0,       /* 180° 竖屏 */
    0xA0,       /* 270° 横屏 */
};

void bsp_lcd_set_rotation(uint8_t rotation)
{
    if (rotation > 3)
    {
        return;
    }

    lcd_dma_sync();                     /* §11 入口防御：等 DMA 像素流收尾再动总线 */

    lcd_rotation = rotation;
    lcd_write_cmd(CMD_MADCTL);
    lcd_write_data1(rot_madctl[rotation]);
}

/* 直写 MADCTL(0x36) 任意值（教学/实验用，test_lcd_colmajor 的列优先实验）。
 * set_rotation 只能从 rot_madctl[4] 四个常用方向里选；本接口允许任意位组合
 * （如 MV=0 族的镜像变体）。不动 lcd_rotation 变量——逻辑宽高、之后
 * set_rotation 恢复常用方向都不受影响。用完记得 set_rotation 复位 */
void bsp_lcd_set_madctl_raw(uint8_t m)
{
    lcd_dma_sync();                     /* §11 入口防御：等 DMA 像素流收尾再动总线 */
    lcd_write_cmd(CMD_MADCTL);
    lcd_write_data1(m);
}

uint16_t bsp_lcd_width(void)
{
    return (lcd_rotation & 1) ? LCD_NATIVE_H : LCD_NATIVE_W;
}

uint16_t bsp_lcd_height(void)
{
    return (lcd_rotation & 1) ? LCD_NATIVE_W : LCD_NATIVE_H;
}

/* 接线自检：把 CS/SCK/SDA/RS/RST 全部打成静态电平（0/0/1/1/1），
 * 供万用表在 FPC 侧逐根核对（SCK/SDA 需先从复用改成普通输出） */
void bsp_lcd_wiring_check(void)
{
    GPIO_InitTypeDef g = {0};

    RCC_PB2PeriphClockCmd(RCC_PB2Periph_GPIOA | RCC_PB2Periph_GPIOC, ENABLE);

    g.GPIO_Pin   = LCD_CS_PIN | LCD_SCK_PIN | LCD_MOSI_PIN | LCD_RST_PIN;
    g.GPIO_Speed = GPIO_Speed_High;
    g.GPIO_Mode  = GPIO_Mode_Out_PP;
    GPIO_Init(GPIOA, &g);

    g.GPIO_Pin = LCD_DC_PIN;
    GPIO_Init(LCD_DC_PORT, &g);

    LCD_CS_LOW();                              /* PA4 -> FPC pin5  CS    = 0V   */
    GPIOA->BCR  = LCD_SCK_PIN;                 /* PA5 -> FPC pin3  SCK   = 0V   */
    GPIOA->BSHR = LCD_MOSI_PIN;                /* PA7 -> FPC pin6  SDA   = 3.3V */
    LCD_DC_HIGH();                             /* PC3 -> FPC pin4  RS    = 3.3V */
    LCD_RST_HIGH();                            /* PA0 -> FPC pin2  RESET = 3.3V */
}

int bsp_lcd_spi_loopback(void)
{
    /* 固定测试图案：全 0/全 1/交叠/行走 1/递变，覆盖常见接线错位 */
    static const uint8_t pat[] = {
        0x00, 0xFF, 0xAA, 0x55, 0x0F, 0xF0, 0x81, 0x7E,
        0x12, 0x34, 0x56, 0x78, 0x9C, 0xB3, 0xD6, 0xE9,
    };
    uint32_t i;
    int bad = 0;

    lcd_dma_sync();                     /* §11 入口防御：自环要重配 SPI，先等 DMA 收尾 */

    spi1_reconfig(LCD_SPI_WRITE_PRESCALER);        /* 与写屏同速 */
    (void)SPI_I2S_ReceiveData(SPI1);               /* 清掉可能残留的 RX */

    for (i = 0; i < sizeof(pat) / sizeof(pat[0]); i++)
    {
        uint8_t echo = spi1_xfer_byte(pat[i]);     /* 发 pat[i]，收回显 */

        if (echo != pat[i])
        {
            bad++;
            uart_printf("  loopback: sent %x got %x\r\n", pat[i], echo);
        }
    }

    return bad;
}

uint8_t bsp_lcd_read_reg(uint8_t cmd, uint8_t dummy, uint8_t *buf, uint8_t len)
{
    /* 保守默认档 /128；要别的读速用 _p 变体（test_lcd_gscan 的频率梯） */
    return bsp_lcd_read_reg_p(cmd, dummy, buf, len, SPI_BaudRatePrescaler_128);
}

/* 读回内核（分频可选）：诊断读 + GSCAN 高频轮询共用。
 * presc 传 SPI_BaudRatePrescaler_x；快档能不能读对由调用方实验判定
 * （SDO 回读时序裕量随档位变小，/128 是板证安全档） */
uint8_t bsp_lcd_read_reg_p(uint8_t cmd, uint8_t dummy, uint8_t *buf, uint8_t len,
                           uint16_t presc)
{
    uint8_t i;

    if ((buf == 0) || (len == 0))
    {
        return 0;
    }

    lcd_dma_sync();                     /* §11 入口防御：读回要降速重配，先等 DMA 收尾 */

    /* 读回时序裕量小，降速再读，读完恢复写速度 */
    spi1_reconfig(presc);
    (void)SPI_I2S_ReceiveData(SPI1);               /* 清掉可能残留的 RX */

    LCD_DC_LOW();                                  /* 命令阶段 DC 低 */
    LCD_CS_LOW();
    spi1_send_byte(cmd);
    spi1_wait_idle();
    LCD_DC_HIGH();                                 /* 参数阶段 DC 高 */

    for (i = 0; i < dummy; i++)                    /* 空时钟，等面板吐数 */
    {
        (void)spi1_xfer_byte(0xFF);
    }
    for (i = 0; i < len; i++)
    {
        buf[i] = spi1_xfer_byte(0xFF);
    }

    LCD_CS_HIGH();
    spi1_reconfig(LCD_SPI_WRITE_PRESCALER);
    return len;
}

/* ==================== TE（撕裂效应）命令（探索实验，2026-10-15） ====================
 * ST7789 命令集：TEOFF(0x34) / TEON(0x35) / SETSCR(0x44)。本模组 FPC 没有
 * 焊出 TE 引脚（pin15~18 NC）——TE 是面板→MCU 的纯电平输出，打了命令也
 * 只打在没接的 pad 上，电学上永远测不到。但仍值得实验的原因：TE 模式的
 * 开/关状态记录在 RDDST(0x09) 状态字里，能走 SDO(pin7)→PA6 读回来验证
 * 命令真的进了面板。入口防御同 set_rotation：先等 DMA 像素流收尾 */
void bsp_lcd_te_on(uint8_t mode)
{
    lcd_dma_sync();
    lcd_write_cmd(0x35);                       /* TEON */
    lcd_write_data1(mode != 0 ? 1u : 0u);      /* 0=仅V-blanking 1=V+H blanking */
}

void bsp_lcd_te_off(void)
{
    lcd_dma_sync();
    lcd_write_cmd(0x34);                       /* TEOFF，无参数 */
}

void bsp_lcd_te_set_scanline(uint16_t line)
{
    lcd_dma_sync();
    lcd_write_cmd(0x44);                       /* SETSCR：TE 脉冲发生在第几行 */
    lcd_write_data1((uint8_t)(line >> 8));     /* 10bit 有效，高 2 位先发 */
    lcd_write_data1((uint8_t)(line & 0xFFu));
}

void bsp_lcd_init(void)
{
    spi1_init();

    /* 硬复位：datasheet 复位脉宽 >9us、释放后等 5ms，取保守毫秒级 */
    LCD_RST_LOW();
    lcd_delay_ms(20);
    LCD_RST_HIGH();
    lcd_delay_ms(120);

    lcd_write_cmd(CMD_SLPOUT);                      /* 退出睡眠，之后要求 ~120ms */
    lcd_delay_ms(120);

    lcd_write_cmd(CMD_MADCTL);                      /* 方向按 rot_madctl[]，默认竖屏 */
    lcd_write_data1(rot_madctl[lcd_rotation]);

    lcd_write_cmd(CMD_COLMOD);                      /* 0x55 = 16bit RGB565 */
    lcd_write_data1(0x55);
    lcd_delay_ms(10);

    lcd_write_cmd(CMD_INVON);                       /* IPS 屏通常需要；负片感则改 INVOFF */

    lcd_write_cmd(CMD_DISPON);                      /* 开显示 */
    lcd_delay_ms(50);

    lcd_dma_init();                                 /* §11：DMA 通道一次配置（SPI 请求源打开，
                                                    * 通道 EN=0 不会自己动，CPU 直写不受影响） */

    /* 背光由外部恒流源直接驱动 LEDA/LEDK，软件无步骤（摘要 §7） */
}

void bsp_lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    uint8_t buf[4];

    lcd_dma_sync();                     /* §11 入口防御：新窗口命令不能打断在飞像素流 */

    lcd_write_cmd(CMD_CASET);                       /* 列地址 X */
    buf[0] = (uint8_t)(x0 >> 8);
    buf[1] = (uint8_t)x0;
    buf[2] = (uint8_t)(x1 >> 8);
    buf[3] = (uint8_t)x1;
    lcd_write_data(buf, 4);

    lcd_write_cmd(CMD_RASET);                       /* 行地址 Y */
    buf[0] = (uint8_t)(y0 >> 8);
    buf[1] = (uint8_t)y0;
    buf[2] = (uint8_t)(y1 >> 8);
    buf[3] = (uint8_t)y1;
    lcd_write_data(buf, 4);

    lcd_write_cmd(CMD_RAMWR);                       /* 之后写入的都进 GRAM */
}

/* ---------- 10.2 连续窗口传输 ----------
 * 一次矩形填充整段只拉一次 CS、结尾才释放，中间只翻 DC：
 *   CS↓ → DC↓CASET → DC↑4B → DC↓RASET → DC↑4B → DC↓RAMWR → DC↑像素流 → CS↑
 * ST7789 只在每个字节的采样时刻看 DC 电平，CS 全程拉低跨 DC 翻转合法。
 * DC 每次翻转前照旧 wait_idle（规则不变）；对外 set_window/write_pixels
 * 的旧时序保持不变，组合路径仅供 fill 内部使用 */
static void lcd_window_begin(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    LCD_CS_LOW();
    LCD_DC_LOW();                                   /* CASET 命令 */
    spi1_send_byte(CMD_CASET);
    spi1_wait_idle();
    LCD_DC_HIGH();                                  /* CASET 参数 */
    spi1_send_byte((uint8_t)(x0 >> 8));
    spi1_send_byte((uint8_t)x0);
    spi1_send_byte((uint8_t)(x1 >> 8));
    spi1_send_byte((uint8_t)x1);
    spi1_wait_idle();
    LCD_DC_LOW();                                   /* RASET 命令 */
    spi1_send_byte(CMD_RASET);
    spi1_wait_idle();
    LCD_DC_HIGH();                                  /* RASET 参数 */
    spi1_send_byte((uint8_t)(y0 >> 8));
    spi1_send_byte((uint8_t)y0);
    spi1_send_byte((uint8_t)(y1 >> 8));
    spi1_send_byte((uint8_t)y1);
    spi1_wait_idle();
    LCD_DC_LOW();                                   /* RAMWR 命令 */
    spi1_send_byte(CMD_RAMWR);
    spi1_wait_idle();
    LCD_DC_HIGH();                                  /* 之后进入像素流，CS 保持拉低 */
}

static void lcd_window_end(void)
{
    spi1_wait_idle();
    LCD_CS_HIGH();
}

/* ---------- 10.3 SPI 16bit 像素流 ----------
 * 命令阶段保持 8bit（CASET/RASET 是 5 字节奇数序列，无法 16bit 打包），
 * 像素流切换 16bit：一次发送一个完整 RGB565。MSB first 使 16bit 帧的高
 * 字节先上线，与原来 hi/lo 两个 8bit 帧的线序完全一致。
 * DFF 位只能在 SPI 关闭（SPE=0）时修改，切换点都在总线空闲处。 */
static void spi1_datasize_16(void)
{
    SPI_Cmd(SPI1, DISABLE);
    SPI_DataSizeConfig(SPI1, SPI_DataSize_16b);
    SPI_Cmd(SPI1, ENABLE);
}

static void spi1_datasize_8(void)
{
    SPI_Cmd(SPI1, DISABLE);
    SPI_DataSizeConfig(SPI1, SPI_DataSize_8b);
    SPI_Cmd(SPI1, ENABLE);
}

/* 16bit 帧发送一个半字（像素阶段专用） */
static void spi1_send_half(uint16_t h)
{
    while ((SPI1->STATR & SPI_I2S_FLAG_TXE) == 0)
    {
    }
    SPI1->DATAR = h;
}

/* ---------- 11. SPI1 TX DMA（DMA1_Channel3）像素流 ----------
 * 检查点 A：轮询同步语义。write_pixels 启动 DMA 后原地等 TC，对外接口与
 * CPU 轮询版完全一致（阻塞直到像素全部上线、总线收尾）。
 * DMA 与 16bit SPI 帧直接衔接：半字内存 -> DATAR，MSB first 线序与 CPU
 * 版逐位相同，面板无感知。通道映射依据 WCH EVT 例程 SPI_DMA：
 * SPI1_TX = DMA1_Channel3（RX 是 Channel2），时钟挂在 RCC_HBPeriph_DMA1。 */
#define LCD_DMA_CH          DMA1_Channel3
#define LCD_DMA_TC_TIMEOUT  1000000UL     /* 轮询超时环数（百 ms 量级） */

static volatile uint8_t s_dma_inflight;   /* start 置 1，收尾清 0 */
static uint32_t s_dma_start_cnt, s_dma_done_cnt, s_dma_err_cnt;
static uint32_t s_dma_fallback_cnt;    /* 异步入口降级同步的累计次数（getter 注释见下） */

/* DMA 时钟 + 通道配置（bsp_lcd_init 尾部调一次）：
 * 半字/半字、MINC 开、PINC 关、Normal 模式、高优先级、外设口 SPI1->DATAR */
static void lcd_dma_init(void)
{
    DMA_InitTypeDef d = {0};

    RCC_HBPeriphClockCmd(RCC_HBPeriph_DMA1, ENABLE);

    DMA_DeInit(LCD_DMA_CH);
    d.DMA_PeripheralBaseAddr = (uint32_t)&SPI1->DATAR;
    d.DMA_MemoryBaseAddr     = 0;
    d.DMA_DIR                = DMA_DIR_PeripheralDST;
    d.DMA_BufferSize         = 0;
    d.DMA_PeripheralInc      = DMA_PeripheralInc_Disable;
    d.DMA_MemoryInc          = DMA_MemoryInc_Enable;
    d.DMA_PeripheralDataSize = DMA_PeripheralDataSize_HalfWord;
    d.DMA_MemoryDataSize     = DMA_MemoryDataSize_HalfWord;
    d.DMA_Mode               = DMA_Mode_Normal;
    d.DMA_Priority           = DMA_Priority_High;
    d.DMA_M2M                = DMA_M2M_Disable;
    DMA_Init(LCD_DMA_CH, &d);

    /* 打开 SPI 的 DMA 请求源（通道 EN=0 时不产生请求，CPU 直写不受影响） */
    SPI_I2S_DMACmd(SPI1, SPI_I2S_DMAReq_Tx, ENABLE);
}

/* 启动一次传输。gdb 实测（CFGR3 读数）：WCH 的 Normal 模式传完 CNTR 归零，
 * 但 EN 位不自动清（与 STM32F1 手册描述不同）。本引擎每次启动前总是先禁
 * 通道再改写 CNTR/MADDR，两种行为下都安全 */
static void lcd_dma_start(const uint16_t *px, uint16_t n)
{
    DMA_Cmd(LCD_DMA_CH, DISABLE);
    LCD_DMA_CH->MADDR = (uint32_t)px;
    LCD_DMA_CH->CNTR  = n;
    DMA_ClearFlag(DMA1_FLAG_GL3);          /* 清残留标志，防上次 TC 误判 */
    s_dma_inflight = 1;
    s_dma_start_cnt++;
    DMA_Cmd(LCD_DMA_CH, ENABLE);
}

/* 总线收尾（完成/出错/超时共用）：等移位寄存器真正空 -> 释放 CS -> DFF 回 8bit */
static void lcd_dma_bus_epilogue(void)
{
    spi1_wait_idle();
    LCD_CS_HIGH();
    spi1_datasize_8();
    s_dma_inflight = 0;
    s_dma_done_cnt++;
}

/* 轮询等 TC，有界防挂死；TE 或超时返回 1（调用方负责收尾） */
static int lcd_dma_wait_tc(void)
{
    uint32_t guard = LCD_DMA_TC_TIMEOUT;

    while (DMA_GetFlagStatus(DMA1_FLAG_TC3) == RESET)
    {
        if (DMA_GetFlagStatus(DMA1_FLAG_TE3) != RESET)
        {
            s_dma_err_cnt++;
            DMA_Cmd(LCD_DMA_CH, DISABLE);
            return 1;
        }
        if (--guard == 0)
        {
            s_dma_err_cnt++;
            DMA_Cmd(LCD_DMA_CH, DISABLE);
            return 1;
        }
    }
    return 0;
}

/* 入口防御：若有在飞传输（异常路径遗留），有界等它结束；超时强停并收尾 */
static void lcd_dma_sync(void)
{
    uint32_t guard = LCD_DMA_TC_TIMEOUT * 2;

    while (s_dma_inflight)
    {
        if (--guard == 0)
        {
            DMA_Cmd(LCD_DMA_CH, DISABLE);
            lcd_dma_bus_epilogue();
            break;
        }
    }
}

/* DMA 传输统计（诊断用）：三参均可传 NULL 忽略。
 * 正常运行判据：start == done 且 err == 0 */
void bsp_lcd_dma_stats(uint32_t *start, uint32_t *done, uint32_t *err)
{
    if (start != 0)
    {
        *start = s_dma_start_cnt;
    }
    if (done != 0)
    {
        *done = s_dma_done_cnt;
    }
    if (err != 0)
    {
        *err = s_dma_err_cnt;
    }
}

/* 异步入口降级同步的累计次数（task_ui 统计行的 fb、judge 的不变量断言）。
 * 触发条件 = bsp_lcd_write_pixels_async 开头的拒收判定：
 *   count==0 / count>32768（超 DMA 单笔 16 位 CNTR 上限）/ 像素地址奇数
 *   （DMA 半字传输的对齐要求）。
 * 正常应恒为 0：LVGL 部分刷新块最大 320x24=7680 半字，静态画布天然
 * 偶地址。什么时候可能动：换全屏渲染模式（块会超 32768）、画布改成
 * 动态分配（可能奇地址）——动了先看这里再查别的 */
uint32_t bsp_lcd_dma_fallback_cnt(void)
{
    return s_dma_fallback_cnt;
}

/* ---------- 11.5 异步 DMA（检查点 B） ----------
 * TCIE + PFIC 开启后，传输完成由 DMA1_Channel3_IRQHandler 收尾：
 * 清标志 -> 总线收尾 -> 回调（回调里给信号量，任务端阻塞等待，CPU 让出）。
 * 开启后 write_pixels 的同步语义仍保留（改等 ISR 收尾而非轮询 TC 标志），
 * 作为 write_pixels_async 的回退路径继续可用 */
static bsp_lcd_dma_cb_t s_dma_cb;       /* TC 收尾回调，ISR 上下文执行 */
static uint8_t s_dma_irq_mode;          /* 1 = 中断模式已开启 */

/* DMA1_Channel3 的中断服务体（User/ch32v4x7_it.c 的 ISR 只调这一个）。
 * 规矩：不打印、不动态分配、不调任何会阻塞的东西 */
void bsp_lcd_dma_irq_handler(void)
{
    DMA_ClearITPendingBit(DMA1_IT_GL3);
    lcd_dma_bus_epilogue();
    if (s_dma_cb != 0)
    {
        s_dma_cb();
    }
}

/* 使能 DMA 完成中断并注册回调（整个工程只调一次，bsp_lvgl_port_init 里） */
void bsp_lcd_dma_async_enable(bsp_lcd_dma_cb_t cb)
{
    
    lcd_dma_sync();
    s_dma_cb = cb;
    DMA_ClearFlag(DMA1_FLAG_GL3);
    DMA_ITConfig(LCD_DMA_CH, DMA_IT_TC, ENABLE);     /* 全工程唯一开 TCIE 的地方 */
    NVIC_SetPriority(DMA1_Channel3_IRQn, 0x40);      /* 4 位优先级取 4，中等 */
    NVIC_EnableIRQ(DMA1_Channel3_IRQn);
    s_dma_irq_mode = 1;
}

/* 异常/超时兜底：强停通道并收总线（err 计数），供等待超时路径调用 */
void bsp_lcd_dma_recover(void)
{
    DMA_Cmd(LCD_DMA_CH, DISABLE);
    s_dma_err_cnt++;
    lcd_dma_bus_epilogue();
}

/* 异步启动像素流：返回时 DMA 在飞、总线未收尾（收尾在中断里做）。
 * 返回 0 = 已异步启动，完成经 async_enable 注册的回调通知；
 * 返回 1 = 条件不满足走了同步路径（返回时已发完收完），调用方需自行
 *   处理"完成"通知 —— LVGL 场景即补一次 lv_display_flush_ready。
 * 异步条件：count<=32768（不分块）且地址半字对齐（DMA 半字传输要求） */
int bsp_lcd_write_pixels_async(const uint16_t *pixels, uint32_t count)
{
    if ((count == 0) || (count > 32768) || (((uint32_t)pixels & 1u) != 0))
    {
        s_dma_fallback_cnt++;                        /* 降级计数：正常应恒 0 */
        bsp_lcd_write_pixels(pixels, count);         /* 同步回退（含全部收尾） */
        return 1;
    }

    lcd_dma_sync();                     /* 防御：等上一次（若有）收尾 */
    LCD_DC_HIGH();
    LCD_CS_LOW();
    spi1_datasize_16();                 /* 像素流 16bit（与同步版一致） */

    lcd_dma_start(pixels, (uint16_t)count);   /* inflight 先置 1 再使能通道 */
    return 0;
}

void bsp_lcd_write_pixels(const uint16_t *pixels, uint32_t count)
{
    lcd_dma_sync();                     /* 入口防御：等残留传输收尾 */

    LCD_DC_HIGH();
    LCD_CS_LOW();
    spi1_datasize_16();                 /* 像素流切 16bit：DMA 半字直接对应
                                         * 一个完整 RGB565 帧，线序与 CPU 版一致 */

    while (count != 0)
    {
        uint16_t chunk = (count > 32768) ? 32768 : (uint16_t)count;
        /* CNTR 16 位上限 65535，此处分块纯接口健壮性；
         * 实际 LVGL 块 <=320*24=7680 半字，永远一块传完 */

        lcd_dma_start(pixels, chunk);
        if (s_dma_irq_mode)
        {
            /* 中断模式：TC 标志会被 ISR 抢先清掉，不能轮询——等 ISR 的
             * epilogue 清 s_dma_inflight（本函数只在任务/主线程上下文跑） */
            uint32_t guard = LCD_DMA_TC_TIMEOUT * 2;

            while (s_dma_inflight)
            {
                if (--guard == 0)
                {
                    s_dma_err_cnt++;
                    DMA_Cmd(LCD_DMA_CH, DISABLE);    /* 中断丢失：放弃剩余块 */
                    break;
                }
            }
            if (guard == 0)
            {
                break;
            }
        }
        else if (lcd_dma_wait_tc() != 0)
        {
            break;                      /* 出错/超时：错误已计数，放弃剩余块 */
        }
        pixels += chunk;
        count -= chunk;
    }

    lcd_dma_bus_epilogue();
}

void bsp_lcd_fill(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint16_t color)
{
    uint32_t total = (uint32_t)(x1 - x0 + 1) * (uint32_t)(y1 - y0 + 1);
    uint32_t  i;

    lcd_dma_sync();                     /* §11 入口防御：fill 仍走 CPU 轮询，先等 DMA 收尾 */

    lcd_window_begin(x0, y0, x1, y1);  //写入的窗口（整段一个 CS 周期，8bit 命令）
    spi1_datasize_16();                //像素流切 16bit：一次发一个完整 RGB565
    for (i = 0; i < total; i++)
    {
        spi1_send_half(color);
    }
    lcd_window_end();                  //wait_idle 后释放 CS（标志位在 16bit 下语义相同）
    spi1_datasize_8();                 //切回 8bit 供下一次窗口命令
}
