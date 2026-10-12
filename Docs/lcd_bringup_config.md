# LCD 点亮配置记录（P032B017P / ST7789P3）

> 状态：**2026-09-24 已点亮验证通过**（根因：面板 RESET 曾接错 MCU 引脚，悬空导致
> 控制器一直处于复位态，所有命令被无视、SDO 高阻——详见 §8 踩坑记录）。
>
> 相关文件：驱动 `Bsp/bsp_lcd.c/.h`，点屏测试 `Test/test_bsp_lcd.c`，
> 硬件规格摘要在仓库目录 `P032B017P_ST7789P3_LVGL_Hardware_Summary (1).md`。

---

## 1. 时钟链（从晶振到 SPI 时钟）

```
HSE 晶振 25 MHz
   │  PLL
   ▼
SYSCLK 400 MHz                     (system_ch32v4x7.c: SYSCLK_400MHz_HCLK_200MHz_HSE)
   │  AHB ÷2
   ▼
HCLK  200 MHz  = SystemCoreClock   (CPU/内存/PFIC)
   │  APB2 ÷1
   ▼
PCLK2 200 MHz                      (SPI1、USART1 所在总线)
   │  SPI1 波特率预分频 ÷32
   ▼
SCK 6.25 MHz                       (写屏速度；读回时自动降为 ÷128 → 1.5625 MHz)
```

- 修改时钟选型：`User/system_ch32v4x7.c` 顶部 `#define`（当前 400M/200M，注释里还有 120/240/350/480 组合）
- FreeRTOS tick：SysTick 500 Hz（与本文无直接关系，仅说明调度环境）

## 2. 引脚映射（已实物验证）

| MCU 引脚 | 方向 | FPC 引脚 | 信号 | 说明 |
|---|---|---|---|---|
| PA4 | 输出(软件) | pin5 | CS | 片选，低有效，软件控制，事务前后拉低/抬高 |
| PA5 | AF 推挽 | pin3 | SCK | SPI1 时钟，Mode0 |
| PA7 | AF 推挽 | pin6 | SDA(MOSI) | 命令/参数/像素数据唯一进线 |
| PA6 | 浮空输入 | pin7 | SDO(MISO) | 仅诊断读回用（RDDID/RDDST），纯显示可不接 |
| PC3 | 推挽输出 | pin4 | RS/DC | 低=命令，高=数据/参数 |
| PA0 | 推挽输出 | pin2 | RESET | 低有效总复位 |
| — | — | pin9 | VCC | 2.8 V（面板逻辑+模拟电源） |
| — | — | pin1/pin8 | GND | 地 |
| — | — | pin10/pin11~14 | LEDA/LEDK | 背光 LED 正/负极，外部恒流 ~90 mA，**模组无背光使能脚** |

## 3. SPI1 配置

| 项 | 值 | 备注 |
|---|---|---|
| 外设 | SPI1（APB2） | 主模式 |
| 模式 | CPOL=Low / CPHA=1Edge（Mode0） | SCK 上升沿采样，符合 ST7789 要求 |
| 数据宽度 | 8 bit | |
| 位序 | MSB first | |
| NSS | 软件（PA4 独立 GPIO） | |
| 方向 | 2 线全双工 | 为 SDO 读回诊断保留；纯写不收 |
| 写速度 | **6.25 MHz**（PCLK2÷32） | 保守起步值（preliminary datasheet 无官方 SCK 上限，摘要 §9.3） |
| 读速度 | 1.5625 MHz（÷128，`bsp_lcd_read_reg` 内自动切换、读完恢复） | 读回时序裕量小 |

事务纪律（防止错位的关键细节）：
- 每字节发送前等 `TXE`；
- **抬 CS / 切换 DC 之前必须等 `TXE && !BSY`**（`spi1_wait_idle`），否则最后一两位还
  在移位寄存器里，DC 电平一变会被面板误判成命令/数据。

## 4. 初始化序列（`bsp_lcd_init`，点亮流程）

| 步骤 | 操作 | 参数/时序 | 作用 |
|---|---|---|---|
| 1 | GPIO/SPI1 初始化 | 见 §2 §3 | 总线就绪，CS/RST 先拉高 |
| 2 | 硬复位 | RST 低 20ms → 高 → 等 120ms | 芯片复活、寄存器回默认（datasheet: >9µs 脉宽 + 释放后 ≥5ms，取保守值） |
| 3 | `SLPOUT (0x11)` | 等 120ms | 退出睡眠，内部电源/振荡启动 |
| 4 | `MADCTL (0x36)` | 0x00 | 竖屏、RGB 顺序（红蓝颠倒改 0x08） |
| 5 | `COLMOD (0x3A)` | 0x55 | 像素格式 RGB565 |
| 6 | `INVON (0x21)` | — | IPS 极性反转（负片感则改 INVOFF 0x20） |
| 7 | `DISPON (0x29)` | 等 50ms | 打开显示 |

背光无软件步骤（外部恒流直供 LEDA/LEDK）。

## 5. 刷屏命令与数据格式

每次刷新任意矩形区域（`bsp_lcd_fill` / `bsp_lcd_set_window`+`bsp_lcd_write_pixels`）：

```
CASET (0x2A) + x0>>8,x0,x1>>8,x1     X 窗口（大端）
RASET (0x2B) + y0>>8,y0,y1>>8,y1     Y 窗口（大端）
RAMWR (0x2C)                          之后全是像素数据
→ 像素数 × 2 字节，RGB565 高字节在前
```

- GRAM 地址指针在窗口内**自动递增+回卷**，MCU 不需要逐像素发地址
- 像素定义：`LCD_COLOR(r,g,b)` = `(r&0xF8)<<8 | (g&0xFC)<<3 | b>>3`
- 面板内部扫描率 ≈60 Hz（FRCTRL2 默认 RTNA=0x0F，未改）

### 5.1 旋转（横竖屏）支持

驱动提供方向 API（`bsp_lcd_set_rotation` / `bsp_lcd_width` / `bsp_lcd_height`）：

| 方向 | MADCTL | 逻辑分辨率 |
|---|---|---|
| `LCD_ROT_PORTRAIT`（默认） | 0x00 | 240×320 |
| `LCD_ROT_LANDSCAPE` | 0x60 | 320×240 |
| `LCD_ROT_PORTRAIT_180` | 0xC0 | 240×320 |
| `LCD_ROT_LANDSCAPE_270` | 0xA0 | 320×240 |

原理要点（踩过的坑）：**MADCTL 的 MV 位会同时交换 CASET/RASET 的坐标轴**——
横屏后 CASET 沿长边（0..319）、RASET 沿短边（0..239）。驱动的 set_window/fill
统一接收**当前方向的逻辑坐标**（横屏即 x:0..319, y:0..239），轴交换由控制器完成，
调用方（含将来 LVGL flush_cb）无感知。若红蓝颠倒：`bsp_lcd.c` 的 `rot_madctl[]`
每个值 `|0x08`。

## 6. 速度与刷新率分析

以当前 **6.25 MHz** 轮询发送计算（纯像素负载，未计 CS/DC 切换与轮询间隙）：

| 场景 | 数据量 | 耗时 | 等效帧率 |
|---|---|---|---|
| 全屏 240×320 | 153 600 B (150 KiB) | **≈197 ms** | **≈5 FPS** |
| 半屏 240×160 | 76 800 B | ≈98 ms | — |
| LVGL 典型块 240×40 | 19 200 B | ≈24.6 ms | — |
| 一行 240 | 480 B | ≈0.61 ms | — |

对照目标（摘要 §17）：全屏 30 FPS 需 ≥36.9 Mbps，60 FPS 需 ≥73.7 Mbps —— 当前
6.25 Mbps 远不够，这正是后续优化的量化依据（§7）。

提速选项（改 `bsp_lcd.c` 里 `spi1_reconfig` 的预分频，需先长时间稳定性验证）：

| 预分频 | SCK | 全屏耗时 |
|---|---|---|
| ÷32（当前） | 6.25 MHz | 197 ms |
| ÷16 | 12.5 MHz | 98 ms |
| ÷8 | 25 MHz | 49 ms |
| ÷4 | 50 MHz | 24.6 ms |

## 7. 数据传输策略与刷屏策略

**当前（已验证）：**
- **轮询发送**（CPU 忙等 TXE/BSY），无 DMA、无中断；传输期间 CPU 被独占
- **无 MCU 全屏 framebuffer**：面板 GRAM 就是显存，`fill` 用 2 字节循环直接流式发送
- 刷屏粒度 = 任意矩形窗口（测试里用全屏纯色轮播 + 8 竖条验证）

**规划（LVGL 集成时，按摘要 §19~§23）：**
1. **SPI DMA** 替换 `bsp_lcd_write_pixels` 内部实现（接口不变），传输期间释放 CPU
2. **LVGL partial render + 双 draw buffer**：建议 240×40×2 = 18.75 KiB/个 ×2
3. flush_cb 只依赖 `bsp_lcd_set_window` + DMA 版 `bsp_lcd_write_pixels`
4. 稳定后按 §6 提速 SPI
5. 不做全帧 buffer（300 KiB 双缓冲超出 RAM 预算，也无必要）

## 8. 踩坑记录（诊断阶梯，供下次 bring-up 复用）

| 现象 | 根因 | 教训 |
|---|---|---|
| 屏全黑 + 背光亮 + RDDID/RDDST 读回全 0x00 | **RESET 接错→RESX 悬空→芯片一直在复位态**，命令全被无视、SDO 高阻 | 背光亮≠面板活：背光走 LEDA/LEDK 独立供电。复位线是第 0 前提 |
| 曾怀疑 PB10 背光使能 | 该模组**没有背光使能脚** | 看引脚表，别臆造引脚 |
| 驱动曾默认 DC=PB0/RST=PB1 | 实际接线 PC3/PA0 | 引脚宏集中在 `bsp_lcd.h`，接完线先核对这个文件 |

测试文件里保留了三级诊断（`Test/test_bsp_lcd.c`）：
- **step0** `PSG_TEST_LCD_WIRING`：5 线打静态电平（CS/SCK=0V，SDA/RS/RST=3.3V），万用表在 FPC 侧核对
- **step1** `bsp_lcd_spi_loopback`：短接 PA7↔PA6 验证 SPI1 本身（与屏无关）
- **step2** `bsp_lcd_read_reg`：读 RDDID(0x04)/RDDST(0x09)，判断面板是否应答/是否在睡眠

## 9. 当前遗留调参点（点色不对时改）

| 症状 | 改法 |
|---|---|
| 红蓝颠倒 | `bsp_lcd.c` 的 `rot_madctl[]` 各值 `\|0x08` |
| 负片感 | `INVON(0x21)` ↔ `INVOFF(0x20)` |
| 提速 | §6 预分频表 |

点亮验证完成后记得把 `Config/test_config.h` 里 `PSG_TEST_BSP_LCD`、
`PSG_TEST_LCD_WIRING` 置 0（测试整体编译为空，省 ~6s 开机延迟）。
