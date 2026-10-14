# FT6336U 触摸驱动：软 I2C → I3C legacy 模式移植记录（踩坑 + 速率分析）

- **日期**：2026-10-13 板证通过（坐标流恢复正常）
- **硬件**：CH32V407RET（HCLK=200MHz）+ P032B017P-IPS-CTP（FT6336U，7bit 地址 0x38）
- **接线**：PB4=SCL、PB5=SDA、PA8=INT（未用）、PC4=RST
- **代码**：`Bsp/bsp_touch.c/.h`（I3C legacy I2C 传输层）、`Test/test_bsp_touch.c`（用例）
- **姊妹文档**：`Docs/FT6336U_触摸控制器_I2C通信协议与接入说明.md`（协议权威）

---

## 0. 结论速览

| 问题 | 一句话答案 |
|---|---|
| 最终改了什么 | 读寄存器从"RESTART 链复合读"改成**两段独立 STOP 事务**（`S 0x70 reg P` + `S 0x71 N字节 P`） |
| 为什么这样就好了 | 帧处于 ACTIVE 态（RESTART 收尾）时，**第二次写 CTLR 不被硬件受理**——读段根本没上总线；改成独立 STOP 后每段都从 IDLE 起帧，每笔 CTLR 都是"第一笔" |
| 要外部上拉吗 | **当前不要**。外设内部上拉（`I3C_Ctrl_Config(PullUp=ENABLE)`）板证在 390kHz 下稳定够用 |
| I3C 最低 390kHz？ | 是。TIMINGR0 时段字段 8 位、单位 `(N+1)×tHCLK=5ns`，全 0xFF → 半周期 1.28µs → **SCL≈390.6kHz 是数学下限**，回不到 100kHz |
| 390 vs 400kHz 危险吗 | 频率在规格内但只余 2.3%；真正约束是**上升时间 ≤300ns** 而非频率数字。短线板证稳定，见 §5 风险清单 |

---

## 1. 背景：为什么从软 I2C 换到 I3C

软 I2C 版（T1，100kHz 位拍）先跑通了触摸，证明链路、寄存器映射、解码全对。但 PB4/PB5 恰好是 CH32V407 **I3C 的重映射档 1**（`I3C_SCL_1/I3C_SDA_1`，默认档 0 在 PB10/PB11）。I3C 控制器原生支持 **legacy I2C 帧**（`CTLR.MTYPE=0100`，RM 19.3.3.5）——总线上看就是一条普通 I2C，FT6336 完全无感知。好处：时序由外设发生，不受中断抢占影响，CPU 只管填/取 FIFO。

**难点**：WCH EVT 只有一个 I3C 例程（`I3C_DMA_Controller`），全是 I3C private/CCC 传输，**没有任何 legacy I2C 例程**——legacy 模式是对着 RM 寄存器描述首写的。

---

## 2. 最终方案改了什么

### 2.1 文件级

| 文件 | 改动 |
|---|---|
| `Bsp/bsp_touch.c` | 传输层整体重写：位拍原语（start/stop/send/recv）全部删除 → I3C 外设事务 |
| `Bsp/bsp_touch.h` | 地址宏 `FT6336_ADDR_W/R (0x70/0x71)` → `FT6336_I2C_ADDR 0x38`（7bit）；新增 `bsp_touch_dbg_info()` 声明 |
| `Test/test_bsp_touch.c` | 只加失败现场打印；用例主体零改动（API 没动） |

对外 API 完全不变：`bsp_touch_init / read_regs / write_reg / read`。

### 2.2 init 流程（`bsp_touch_init`）

```
时钟：RCC_PB2 (GPIOA|GPIOB|GPIOC|AFIO) + RCC_HB (I3C) → I3C_DeInit
PC4 RST 推挽拉低、PA8 INT 输入上拉（与软 I2C 版相同）
PB4/PB5：GPIO_PinRemapConfig(GPIO_Remap_I3C, ENABLE)  ← 重映射档1
        + GPIO_Mode_AF_PP 交外设驱动
复位时序：RST 低10ms → 高 → 等350ms（手册 Table 3-5，沿用）
I3C_Ctrl_Init：  TIMINGR0 四档全 0xFF（最慢档 ≈390kHz，见 §5）
I3C_Ctrl_Config：PullUp=ENABLE（外设内部上拉）
I3C_SetConfigFifo：RX/TX 阈值 1_4，ControlFifo=DISABLE（同 EVT）
I3C_ArbitrationHeaderCmd(ENABLE)：置 NOARBH=1 → 纯 I2C 帧（见 §3 坑4）
I3C_Cmd(ENABLE) → 探测：完整读一次 FIRMID(0xA6)，失败隔 5ms 重试一次
```

### 2.3 读寄存器：两段独立 STOP 事务（本次结案的关键改动）

```
bsp_touch_read_regs(reg, buf, len):
  第一段（写寄存器号）   S + 0x38(W) + reg + P
  第二段（从当前指针读） S + 0x38(R) + data × len + P
```

- 第一段用 `I3C_ControllerHandleMessage(0x38, 1, WR, LEGACY_I2C, I3C_GENERATE_STOP)` 启动后填 TDBR，**等 FCF**（帧完成）确认走完；
- 第二段启动后**双路收数**：`RXFNEF`（FIFO 有货）为主，`RXTGTENDF`（读满 DCNT 目标）兜底——后者一置位说明整条消息的字节都已进 FIFO，即使没到 FIFO 阈值也可以直接从 RDBR 读走剩余；
- FT6x36 的**寄存器指针跨 STOP 保持**，所以"写 reg(带P) 再读 N 字节(带P)"与复合读语义等价（Arduino FT6206/6236 驱动同款写法）。

为什么不能用复合读（`S 0x70 reg Sr 0x71 data P`）——见 §3 坑 6。

---

## 3. 踩坑全记录（按三块板的顺序）

### 坑 1：速率下限 390kHz（架构性，先知道再动手）

TIMINGR0 四个时段字段全是 8 位，单位 `(N+1) × tHCLK`（RM 19.4.16），HCLK=200MHz → 单位 5ns：

```
SCL 半周期上限 = (0xFF+1) × 5ns = 1.28µs
SCL 周期上限   = 2.56µs → 频率下限 ≈ 390.6kHz
```

**结论：这个外设在 200MHz HCLK 下发不出比 390kHz 更慢的 I2C**。SCL 速度没有别的分频杠杆（RCC 侧查过，I3C 无独立内核时钟分频）。这直接决定了后面"上拉质量"的重要性（§5）。

### 坑 2：重映射（PB4/PB5 不是 I3C 默认脚）

默认档 0 在 PB10/PB11，PB4/PB5 是档 1：必须 `GPIO_PinRemapConfig(GPIO_Remap_I3C, ENABLE)`（写 AFIO_PCFR2 bit14=I3C_RM），且 AFIO 时钟要先开。第一板日志 `PCFR2=0x4000` 确认置位成功。**排查 remap 是否生效就看这个寄存器位**。

### 坑 3：地址是 7bit，不是 I2C 习惯的 8bit

I3C 的 `CTLR.ADD[6:0]` 填 **0x38**，读写方向是独立的 `RNW` 位（RM 帧图 19.3.3.5）。按 I2C 习惯填 0x70/0x71 会实际寻址 0x38<<1=从机 0x70……错地址必 NACK。

### 坑 4：仲裁头 NOARBH（函数名是个陷阱）

I3C 控制器在 legacy 帧前默认可能先发 `7E 保留字节 + Sr`（NOARBH=0 时，RM 19.3.3.5）——对 FT6336 来说就是垃圾前缀。`I3C_ArbitrationHeaderCmd(ENABLE)` 的语义是"**置 NOARBH 位**"=**不**发仲裁头，与直觉的"ENABLE=启用仲裁头"相反。要看实现而不能看函数名。

### 坑 5：填数顺序（第一板失败嫌疑之一）

第一版照 STM32 习惯"先填 TDBR 再启动帧"（preload-then-start）。重读 EVT 发现**WCH 例程从不预填**：DMA 都是 `HandleMessage` 启动帧之后才使能装数，ENTDAA 也是帧进行中才 WriteByte。改为 EVT 顺序：**先 `HandleMessage()` 启动帧，再逐字节等 `TXFNFF` 填 TDBR**（地址相 9 个 SCL ≈23µs，CPU 填数绰绰有余）。
诚实说明：此坑与坑 6 在相邻两板里先后修掉，没能单独归因；但 EVT 顺序是被例程实证的安全序，保留。

### 坑 6：RESTART 链不通（真凶，第二板日志钉死的）

标准 I2C 复合读是 `S 0x70 reg Sr 0x71 data P`。第一版用两段 `HandleMessage` 拼：写段 `I3C_GENERATE_RESTART` 收尾，读段 `I3C_GENERATE_STOP` 收尾。

**第二板日志**（诊断信息加入后）：

```
stage=4（读段等 RXFNEF 超时）
EVR=0x204002 → bit1 TXFEF(发送FIFO空)=1, bit10 RXTGTENDF=1,
              但 bit9 FCF(帧完成)=0, bit11 ERRF=0, bit5 RXFNEF=0
STATER=0x0   → 总线零错误（地址从未被 NACK）
PCFR2=0x4000 → remap 正常
```

解读：写段正常起帧、字节被取走（TXFEF）；RXTGTENDF 置位是**写段自己**达到 DCNT 目标的标志（该位方向无关）——不是读段收到了数据；读段启动后总线**完全静默**（无数据、无错误、无帧完成）。

结合 RM 流程图原话 *"Initiate a frame transfer … write **(first)** R32_I3C_CTLR"*：消息排队依赖 Control FIFO，而本工程按 EVT 配置 `ControlFifo=DISABLE`——**帧处于 ACTIVE 态（RESTART 收尾恰好保持 ACTIVE）时，第二次写 CTLR 被直接丢弃**。读段根本没上总线。

**修复（第三板，成功）**：两段独立 STOP 事务。每段都让帧回到 IDLE（STOP 收尾），下一笔 CTLR 都是"第一笔"。写段也顺势补上了 FCF 等待（RESTART 方案里只确认过"字节离开 FIFO"，从没验证过"帧走完"）。

### 坑 7：诊断方法论（这轮能三板收敛的原因）

每板带**失败现场快照**：stage 码（死在哪步）+ `EVR/STATER/PCFR2/CTLR/STATR`（消息字是否被受理、XDCNT 实际搬运数）。要点：

- 快照要在 `FlushAllFifo()`/清标志**之前**抓，否则现场被销毁；
- 跨事务不清的标志（如 RXTGTENDF）会成为"陈货"误导归因——修复版在每段事务开始时一并清掉；
- 一次只改一个变量，但**诊断信息一次加满**——三板之内从"无反应"收敛到"读段没上总线"。

---

## 4. 为什么当前不需要外部上拉电阻

1. **外设内部上拉是开着的**：`I3C_Ctrl_Config` 的 `PullUp=ENABLE`（CFGR bit6）。I3C 外设本身是为高速开漏段设计的，其内部上拉强度远高于 GPIO 输入弱上拉（具体阻值手册未给，估 1~10kΩ 量级）。
2. **板证**：390kHz 下地址 ACK、寄存器读写、连续坐标流全部正常、无 err——上升沿质量实测达标，这就是最硬的证据。
3. **总线极短**：FT6336 在 LCD FPC 上，走线电容极小（软 I2C 时期 ~几十 kΩ 的 GPIO 弱上拉在 100kHz 都能工作，间接说明 Cb 很小）。RC 小，上升沿自然快。
4. **软 I2C 的成功不等于 I3C 必然成功**：100kHz 的上升时间预算是 1000ns，390kHz 只有 300ns，差 3.3 倍——所以"以前没加上拉也行"不能直接外推，最终以 I3C 板证为准（已通过）。

**何时需要加**：出现间歇性 `i2c_err` 增长、坐标偶发丢失、init 偶发 NACK（`STATER=0x100`）——都是边沿质量恶化的信号。届时 SDA/SCL 各焊一颗 **2.2k~4.7k** 到 3.3V（SDA 必加：从机应答位靠它拉高；SCL 建议一起加）。同一 hex 无需重编即可复测。

---

## 5. 390kHz 下限 vs 400kHz 上限：余量分析

### 数字本身

| 项 | 值 | 来源 |
|---|---|---|
| 我们的 SCL | **390.6kHz**（半周期 2×1.28µs，全 0xFF 档） | TIMINGR0 数学下限，§3 坑1 |
| FT6336 规格上限 | 400kHz | FT 手册 |
| 频率余量 | 9.4kHz / **2.3%** | —— |
| FT6336 规格下限 | 10kHz | 远不构成约束 |

**频率数字在规格内，但贴着上限跑。**

### 真正的约束：上升时间，不是频率

I2C 快速模式（400kHz 档）的隐性要求是 `tr ≤ 300ns`（高电平爬升时间，由 `Rp×Cb` 决定）。频率 390 还是 400 只决定时钟节拍；**数据能不能被正确采样取决于边沿**：

```
tr ≈ 0.847 × Rp(上拉) × Cb(总线电容)
当前：短 FPC（Cb 小）+ 外设内部上拉（Rp 未知但足够）→ tr 实测达标（板证零 err）
```

也就是说：**"390 贴 400"这个频率余量小不是问题本身；问题是 tr 的余量我们看不见、只能靠实测确认——目前实测是好的。**

### 什么场景会出问题（风险清单）

| 场景 | 影响 | 概率 |
|---|---|---|
| 加长连接线/飞线/面包板 | Cb 上升 → tr 恶化 → NACK/数据错 | **最可能**——保持 FPC 直连 |
| 换触控模块个体/批次 | 输入电容、内部上拉差异 | 低 |
| 低温/个体容差 | 边沿进一步变缓 | 低（余量本来就小） |
| 中断风暴导致 CPU 响应慢 | **无影响**——时序在外设硬件里，这正是 I3C 相对软 I2C 的优势 | —— |

### 兜底顺序（未来真出问题时）

1. **外部 2.2k~4.7k 上拉**（tr 直接拉下来，90% 的此类问题到此为止）；
2. 查线材/接触（Cb、串扰）；
3. 降速——**做不到**：硬件下限就是 390kHz（除非动 HCLK，等于改全系统时钟，不可行）；
4. 极端情况回退软 I2C 传输层（对外 API 不变，测试不动）。

**长期健康指标**：测试里 `release` 行的 `i2c_err` 计数（软 I2C 版即有）。坐标流正常 + err 不涨 = 总线健康；err 缓涨 = 边沿质量在恶化，按上表处理。

---

## 6. 三块板时间线（速查）

| 板 | 版本 | 日志关键行 | 结论 |
|---|---|---|---|
| 1 | preload 顺序 + RESTART 链 | 只有两个 ASSERT，无细节 | 失败，无信息 → 加诊断 |
| 2 | EVT 填数顺序 + stage/EVR/STATER/PCFR2 快照 | `stage=4 EVR=0x204002 STATER=0x0 PCFR2=0x4000` | remap✓、无总线错误、读段静默 → 定位 RESTART 链 |
| 3 | 两段独立 STOP + RXTGTENDF 兜底 + CTLR/STATR 快照 | 坐标流恢复 | **通过**（2026-10-13 板证） |
