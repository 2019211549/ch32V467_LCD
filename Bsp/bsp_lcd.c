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
 * PCLK2=200MHz。已试档位：/32=6.25MHz（基线 PASS，点屏目视通过）、
 * /2=100MHz（实验极限，从未目视确认过；GPIO 档位仅 50MHz 级属超规格）。
 * 2026-10-12 屏幕点不亮，按调参点 1 先降回 /32 排除速率因素。
 * 注意：改这里须同步 test_lcd_perf.c 的 PERF_SCK_HZ。
 * init / 自环测试 / 读回恢复 三处同源引用，保证写速度一致 */
#define LCD_SPI_WRITE_PRESCALER   SPI_BaudRatePrescaler_32

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

    lcd_rotation = rotation;
    lcd_write_cmd(CMD_MADCTL);
    lcd_write_data1(rot_madctl[rotation]);
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
    uint8_t i;

    if ((buf == 0) || (len == 0))
    {
        return 0;
    }

    /* 读回时序裕量小，降到 /128 = 1.5625MHz，读完恢复写速度 */
    spi1_reconfig(SPI_BaudRatePrescaler_128);
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

    /* 背光由外部恒流源直接驱动 LEDA/LEDK，软件无步骤（摘要 §7） */
}

void bsp_lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    uint8_t buf[4];

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

void bsp_lcd_write_pixels(const uint16_t *pixels, uint32_t count)
{
    uint32_t i;

    LCD_DC_HIGH();
    LCD_CS_LOW();
    spi1_datasize_16();                /* 像素流切 16bit（同 fill 的 10.3）：
                                        * 像素值整体写入 DATAR，MSB first 高字节
                                        * 先上线，与两个 8bit 帧的线序一致 */
    for (i = 0; i < count; i++)
    {
        spi1_send_half(pixels[i]);
    }
    lcd_window_end();                  /* wait_idle 后释放 CS（时序同 fill）*/
    spi1_datasize_8();                 /* 切回 8bit 供下一次窗口命令 */
}

void bsp_lcd_fill(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint16_t color)
{
    uint32_t total = (uint32_t)(x1 - x0 + 1) * (uint32_t)(y1 - y0 + 1);
    uint32_t  i;

    lcd_window_begin(x0, y0, x1, y1);  //写入的窗口（整段一个 CS 周期，8bit 命令）
    spi1_datasize_16();                //像素流切 16bit：一次发一个完整 RGB565
    for (i = 0; i < total; i++)
    {
        spi1_send_half(color);
    }
    lcd_window_end();                  //wait_idle 后释放 CS（标志位在 16bit 下语义相同）
    spi1_datasize_8();                 //切回 8bit 供下一次窗口命令
}
