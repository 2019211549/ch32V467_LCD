/*********************************************************************
 * bsp_lcd.h - P032B017P (ST7789P3, 240x320 IPS, 4-wire SPI) 驱动
 *
 * 硬件摘要：P032B017P_ST7789P3_LVGL_Hardware_Summary.md
 *  - 4-wire SPI：CS / SCK / SDA(MOSI) / RS(DC)，SDO(MISO) 可选
 *  - RGB565（COLMOD 0x55），MSB first，SCK 上升沿采样
 *  - 区域刷新：CASET(0x2A) RASET(0x2B) RAMWR(0x2C)
 *  - VCC=2.8V（电平兼容性见摘要 §6，需供应商确认）
 *  - 背光：模组没有使能脚！LEDA/LEDK 由外部恒流源直接供电（摘要 §7），
 *    MCU 不参与背光控制；以后要 PWM 调光再加驱动板级代码
 *
 * 引脚映射按实际接线改下面的宏即可。
 *********************************************************************/
#ifndef BSP_LCD_H
#define BSP_LCD_H

#include <stdint.h>

/* ==================== 引脚映射（按接线修改这里） ==================== */
#define LCD_CS_PORT      GPIOA
#define LCD_CS_PIN       GPIO_Pin_4      /* PA4  软件 CS      -> 模组 pin5  CS   */
#define LCD_SCK_PIN      GPIO_Pin_5      /* PA5  SPI1-SCK     -> 模组 pin3  SCK  */
#define LCD_MISO_PIN     GPIO_Pin_6      /* PA6  SPI1-MISO    <- 模组 pin7  SDO（可不接）*/
#define LCD_MOSI_PIN     GPIO_Pin_7      /* PA7  SPI1-MOSI    -> 模组 pin6  SDA  */

#define LCD_DC_PORT      GPIOC
#define LCD_DC_PIN       GPIO_Pin_3      /* PC3  -> 模组 pin4  RS/DC */
#define LCD_RST_PORT     GPIOA
#define LCD_RST_PIN      GPIO_Pin_0      /* PA0  -> 模组 pin2  RESET */

/* ==================== 面板参数 ==================== */
/* 原生（玻璃）分辨率，固定不变；随方向变化的逻辑宽高用
 * bsp_lcd_width()/bsp_lcd_height() 取，别拿原生宏当逻辑宽高用 */
#define LCD_NATIVE_W     240             /* 原生短边（X 轴 0..239） */
#define LCD_NATIVE_H     320             /* 原生长边（Y 轴 0..319） */

/* ==================== 屏幕方向 ==================== */
/* LCD_ROT_PORTRAIT      0°   竖屏  240x320（原生）
 * LCD_ROT_LANDSCAPE     90°  横屏  320x240（MADCTL=0x60）
 * LCD_ROT_PORTRAIT_180  180° 竖屏  240x320（0xC0）
 * LCD_ROT_LANDSCAPE_270 270° 横屏  320x240（0xA0）
 * 两个横屏差 180°（哪边朝上）；红蓝颠倒在 bsp_lcd.c 的 rot_madctl[] 加 |0x08 */
#define LCD_ROT_PORTRAIT      0
#define LCD_ROT_LANDSCAPE     1
#define LCD_ROT_PORTRAIT_180  2
#define LCD_ROT_LANDSCAPE_270 3

/* ==================== 实测调参点（点不亮/颜色不对时改） ====================
 * 1. SPI_BaudRatePrescaler  : bsp_lcd.c 里 spi1_init()，PCLK2=200MHz，
 *    /32=6.25MHz 保守起步；稳定后可试 /16(12.5M) /8(25M)（摘要 §9.3 无官方上限）
 * 2. MADCTL(0x36) 的值      : 0x00=RGB；红蓝颠倒改 0x08(BGR)
 * 3. INVON(0x21)/INVOFF(0x20): IPS 屏通常要 INVON；观感反常(负片感)则换
 * ==================================================================== */

/* 常用 RGB565 颜色 */
#define LCD_COLOR(r, g, b)   ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
#define LCD_RED              0xF800
#define LCD_GREEN            0x07E0
#define LCD_BLUE             0x001F
#define LCD_WHITE            0xFFFF
#define LCD_BLACK            0x0000
#define LCD_CYAN             0x07FF
#define LCD_MAGENTA          0xF81F
#define LCD_YELLOW           0xFFE0

/* 初始化：GPIO + SPI1 + ST7789P3 复位/初始化序列（背光外部供电，无软件步骤）。
 * init 后默认竖屏（LCD_ROT_PORTRAIT） */
void bsp_lcd_init(void);

/* 设置屏幕方向（可随时调用，立即生效，只重发 MADCTL 不清屏）。
 * 之后 set_window/fill/write_pixels 的坐标全部按新方向的逻辑坐标系理解，
 * 横屏时逻辑宽高变为 320x240 —— 轴交换由控制器完成，调用方无感知 */
void bsp_lcd_set_rotation(uint8_t rotation);

/* 当前方向的逻辑宽/高（竖屏 240x320，横屏 320x240） */
uint16_t bsp_lcd_width(void);
uint16_t bsp_lcd_height(void);

/* SPI1 自环自检：用杜邦线短接 PA7(MOSI)<->PA6(MISO) 后运行，
 * 发送固定字节序列并核对收到的回显。返回失配字节数，0 = SPI1
 * 配置与驱动收发完全正常（与屏幕无关，纯 MCU 侧证明） */
int bsp_lcd_spi_loopback(void);

/* 接线自检：5 根信号线打成静态电平 CS=0/SCK=0/SDA=1/RS=1/RST=1，
 * 用万用表直流挡在 FPC 侧核对（见 test_bsp_lcd.c 的对照表） */
void bsp_lcd_wiring_check(void);

/* 读回寄存器（诊断用，需接好 SDO pin7 -> PA6）：
 * 发命令 cmd，补 dummy 个空时钟，再读 len 字节到 buf。
 * 返回读到的字节数。读回期间自动降到 /128(1.5625MHz)，读完恢复 */
uint8_t bsp_lcd_read_reg(uint8_t cmd, uint8_t dummy, uint8_t *buf, uint8_t len);

/* 设置写入窗口 [x0,x1]x[y0,y1] 并进入 RAMWR 状态（含 0x2A/0x2B/0x2C）。
 * 坐标为当前方向的逻辑坐标（横屏时 x:0..319, y:0..239） */
void bsp_lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);

/* 连续写 count 个 RGB565 像素（高字节在前）。
 * 像素流走 SPI1 TX DMA（DMA1_Channel3）；检查点 A 为轮询等待完成的阻塞
 * 语义，接口与 CPU 版完全一致。检查点 B 将增加异步版本 */
void bsp_lcd_write_pixels(const uint16_t *pixels, uint32_t count);

/* DMA 传输统计（诊断）：三参均可传 NULL 忽略。
 * 正常运行判据：start == done 且 err == 0 */
void bsp_lcd_dma_stats(uint32_t *start, uint32_t *done, uint32_t *err);

/* 异步入口降级同步的累计次数：恒 0 = "永远在走异步"这个不变量成立
 * （触发条件与何时可能动，见 bsp_lcd.c 里 getter 的注释） */
uint32_t bsp_lcd_dma_fallback_cnt(void);

/* ==================== 检查点 B：DMA 异步 ==================== */
/* DMA 完成回调原型（在 ISR 上下文执行：禁止打印/动态分配/阻塞） */
typedef void (*bsp_lcd_dma_cb_t)(void);

/* 异步启动像素流（窗口命令仍同步发）。返回 0 = DMA 在飞，完成经回调通知；
 * 返回 1 = 条件不满足（超 32768 像素或地址未半字对齐）已走同步路径发完，
 * 调用方需自行处理完成通知（LVGL 场景即补 lv_display_flush_ready） */
int bsp_lcd_write_pixels_async(const uint16_t *pixels, uint32_t count);

/* 开启 DMA 传输完成中断并注册回调（一次性，bsp_lvgl_port_init 里调） */
void bsp_lcd_dma_async_enable(bsp_lcd_dma_cb_t cb);

/* DMA1_Channel3 中断服务体（ch32v4x7_it.c 的 ISR 调它，别处别调） */
void bsp_lcd_dma_irq_handler(void);

/* 强制收总线（异常/超时兜底）：禁通道 + 收尾 + err 计数 */
void bsp_lcd_dma_recover(void);

/* 用纯色填充一块区域 */
void bsp_lcd_fill(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1, uint16_t color);

#endif /* BSP_LCD_H */
