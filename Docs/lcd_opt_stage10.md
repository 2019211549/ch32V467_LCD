# 阶段10 微优化记录（自底向上）— LCD SPI 驱动

> **背景**：按用户节奏先做阶段10（原规格书的第二梯队微优化），从最底层开始。
> 前置事实（阶段0 基线，2026-09-24 实测）：solid 196.6ms = 理论值，总线利用率
> >99.5% —— **6.25MHz 下 solid 已到物理上限，软件优化对它无效**。
> 阶段10 的目标因此是两个：
> 1. **KPI = checker − solid 差值（窗口开销）**：基线 5.3ms/帧 = 300 窗口 × 17.7µs。
>    其中窗口命令 **11 字节**（CASET 1+4、RASET 1+4、RAMWR 1）纯线时间
>    **14.08µs** @6.25MHz，软件部分基线 ≈3.6µs → 目标：软件压到 ~1µs，KPI ≈4.3ms
>    （更正记录：早期文档误写 13 字节/16.64µs，2026-09-24 修正）
> 2. **为提频打底**：50MHz 时字节周期 0.16µs，这些开销会放大成 50% 级损失

每步一个变量，改完烧录跑 `PSG_TEST_LCD_PERF`，数据贴回后进下一步。

---

## 10.1 底层原语寄存器化 ✅ 已实施（2026-09-24，待上板验证）

### 改了什么

全部改动在 `Bsp/bsp_lcd.c`，**只换了实现方式，没动任何时序行为**（写同一批寄存器）：

| 改动点 | 原实现 | 新实现 | 收益原理 |
|---|---|---|---|
| GPIO 翻转（CS/DC/RST，全部调用点） | `GPIO_SetBits/ResetBits`（函数调用+参数检查，~20-40 周期） | 宏 `LCD_CS_LOW/HIGH`、`LCD_DC_LOW/HIGH`、`LCD_RST_LOW/HIGH` → `GPIO->BSHR/BCR` 单条存储指令（2-4 周期） | 每窗口 18 次 GPIO 操作 × 调用开销 |
| `spi1_send_byte` TXE 轮询+写入 | `SPI_I2S_GetFlagStatus` + `SPI_I2S_SendData`（两次库调用） | 直读 `SPI1->STATR & TXE`、直写 `SPI1->DATAR` | 每字节 2 次调用 → 0 次调用 |
| `spi1_wait_idle` | 同上 ×2 | 直访 STATR | 每窗口 6 次 |
| `spi1_xfer_byte`（读回/自环用） | 同上 ×3 | 直访 STATR/DATAR | 冷路径，统一风格 |

覆盖的调用点：`spi1_init` / `lcd_write_cmd` / `lcd_write_data` / `wiring_check` /
`read_reg` / `bsp_lcd_init`（RST 复位脉冲）/ `write_pixels` / `fill`。
`spi1_reconfig` 的 `SPI_Init`（初始化冷路径）保留库调用不动。

### 改进了什么（预期，【理论推算】）

- 每字节发送路径：省 2 次函数调用 ≈ 40-80ns @200MHz；每窗口 ≈ 18 次 GPIO 调用 → 宏
- **@6.25MHz 的可见效果**：窗口软件开销 ~1.1µs → 预计 ~0.3-0.5µs，
  **checker 差值 5.3ms → 预计 5.0~5.2ms**；solid 不变（196.6ms）
- **真正收益在提频后**：50MHz 时字节周期 0.16µs，轮询循环里的函数调用会超过
  总线时间导致 CPU 喂不饱——本步把每字节 CPU 成本压到几十 ns，推迟瓶颈出现
- 附带效果：text 11092 → 10956（−136B，库函数被内联后 gc 掉）

### 验证标准（烧录后对照基线）

| 检查项 | 基线 | PASS 条件 |
|---|---|---|
| solid avg | 196.6ms | 196.1~197.1（不变） |
| bars avg | 196.7ms | ±0.5ms |
| gray avg | 197.1ms | ±0.5ms |
| **checker avg** | **201.9ms** | **≤201.7（KPI 下降）且显示完全一致** |
| 屏幕 | — | 色条/棋盘/灰阶与基线完全一致（时序行为等价，不应有任何变化） |

FAIL（显示异常/数值劣化）→ 本步改动全部在 bsp_lcd.c，git 回退该文件即可。

### 实测结果（2026-09-24 上板，PASS）

```
[PERF] ===== PERF test: SCK=6.25MHz polling LANDSCAPE 320x240 =====
[PERF] theory full-frame payload = 196.6ms (153600B @SCK)
[PERF]    solid n=10 avg=196.6ms min=196.6ms max=196.6ms fps=5.0
[PERF]     bars n=10 avg=196.7ms min=196.7ms max=196.7ms fps=5.0
[PERF]  checker n=10 avg=201.4ms min=201.4ms max=201.4ms fps=4.9
[PERF]     gray n=10 avg=197.0ms min=197.0ms max=197.0ms fps=5.0
[PERF] all patterns done, total 7959ms, no hang.
```

| 图案 | 基线 | 10.1 实测 | Δ |
|---|---|---|---|
| solid | 196.6ms | 196.6ms | 0（总线极限，符合预期）|
| bars | 196.7ms | 196.7ms | 0 |
| gray | 197.1ms | 197.0ms | −0.1 |
| checker | 201.9ms | **201.4ms** | **−0.5** |
| **KPI：checker−solid** | **5.3ms** | **4.8ms** | **−0.5ms（−9.4%）** |

**结论**：窗口开销 17.7 → **16.05 µs/窗口**。按 11 字节命令纯线时间 14.08µs
（早期误记 13 字节，已更正）重算：软件部分 3.6 → **2.0µs**（库调用开销基本消除，
剩余为 6 个事务边界）。显示与基线完全一致，PASS。

---

## 10.2 CS 合并 + 连续窗口传输 ✅ 已实施（2026-09-24，待上板验证）

**改了什么**：`bsp_lcd.c` 新增私有组合原语 `lcd_window_begin()/lcd_window_end()`，
`bsp_lcd_fill` 改走组合路径——一次填充整段只拉一次 CS：
```
CS↓ → DC↓CASET → DC↑4B → DC↓RASET → DC↑4B → DC↓RAMWR → DC↑像素流 → CS↑
```
对外 API 签名不变；`set_window`/`write_pixels` 单独使用仍按旧时序（6 事务版保留）。
DC 每次翻转前照旧 `wait_idle`（规则不变）。

**改进了什么（预期【理论推算】，要诚实）**：每窗口省 10 次 GPIO 写（10.1 后已是
单条存储指令，~5ns/次）→ **@6.25MHz 的 checker 差值预期只降 ≈0~0.1ms**
（10×5ns×300 ≈ 15µs/帧，低于 0.1ms 打印分辨率）。本步的真正价值：
1. 结构性验证"CS 全程拉低跨 DC 翻转"的合法性（风险假设上板确认）
2. 高时钟下 CS 边沿的相对成本被放大，此结构一步到位
3. 这是后续 DMA 帧传输的时序骨架（一次 CS 覆盖整帧行为）

**PASS 条件**：显示与 10.1 完全一致——**重点盯棋盘格 300 个窗口无一错位**
（若 CS 跨 DC 翻转不合法，棋盘格会立刻花）；数值不劣化（checker ≤201.6）。
**FAIL**：任一窗口错位/异色 → 回退本步（fill 恢复走 set_window 旧路径）。

### 实测结果（2026-09-24 上板，PASS）

```
[PERF]    solid n=10 avg=196.6ms min=196.6ms max=196.6ms fps=5.0
[PERF]     bars n=10 avg=196.7ms min=196.7ms max=196.7ms fps=5.0
[PERF]  checker n=10 avg=201.1ms min=201.1ms max=201.1ms fps=4.9
[PERF]     gray n=10 avg=197.0ms min=197.0ms max=197.0ms fps=5.0
[PERF] all patterns done, total 7956ms, no hang.
```

| 图案 | 10.1 | 10.2 实测 | Δ |
|---|---|---|---|
| solid | 196.6ms | 196.6ms | 0 |
| bars | 196.7ms | 196.7ms | 0 |
| gray | 197.0ms | 197.0ms | 0 |
| checker | 201.4ms | **201.1ms** | **−0.3** |
| **KPI：checker−solid** | **4.8ms** | **4.5ms** | **−0.3ms** |

**结论**：窗口开销 → **15.05µs**（软件残余 ≈1.0µs：DC 翻转前必需的 6 次
wait_idle + 循环开销）。CS 全程拉低跨 DC 翻转的假设验证成立——
棋盘格 300 窗口无一错位。PASS。
（−0.3ms 好于"≤0.1ms"的预估：省的不只是 10 次 GPIO 写，还有 5 个事务
边界处的行空闲间隙。）

## 10.3 SPI 16bit 像素流 ✅ 已实施（2026-09-24，待上板验证）

**改了什么**（`bsp_lcd.c`）：
- 新增 `spi1_datasize_16()/8()`：DFF 位只能在 SPE=0 时改，切换包在
  `SPI_Cmd(DISABLE/ENABLE)` 里，切换点都选在总线空闲处（窗口命令后 / CS 释放后）
- 新增 `spi1_send_half()`：16bit 帧直发一个半字
- `bsp_lcd_fill` 像素循环改为：`lcd_window_begin`（8bit 命令）→ 切 16bit →
  每像素一次 `spi1_send_half(color)` → `lcd_window_end` → 切回 8bit
- `write_pixels`/`set_window` 暂不动（当前无调用者，LVGL 接入时再迁移）

**线序等价性**：SPI 配置 MSB first，16bit 帧的高字节先上线 = 原来的 hi/lo
两个 8bit 帧，面板看到的字节流完全相同——**这不是改变传输内容，只是把
两次寄存器写合并成一次**。

**改进了什么（预期【理论推算】）**：CPU 每像素从 2 次 TXE 轮询+2 次写降到
1 次+1 次。**@6.25MHz：solid/checker 数值应完全不变**（总线带宽是瓶颈，
CPU 本来就喂得饱）；收益在提频后兑现——50MHz 时字节周期 0.16µs，
8bit 轮询大概率喂不饱，16bit 把喂料需求减半。

**PASS 条件**：**灰阶渐变完全正常**（字节序试金石：任何高低字节颠倒都会让
灰阶变彩色杂带）+ 纯色色序正确 + 数值与 10.2 持平（±0.2ms）。
**FAIL**：灰阶/颜色异常 → 回退本步（fill 恢复 hi/lo 8bit 循环）。

### 实测结果（2026-09-24 上板，PASS）

```
[PERF]    solid n=10 avg=196.6ms min=196.6ms max=196.6ms fps=5.0
[PERF]     bars n=10 avg=196.7ms min=196.7ms max=196.7ms fps=5.0
[PERF]  checker n=10 avg=201.2ms min=201.2ms max=201.2ms fps=4.9
[PERF]     gray n=10 avg=197.0ms min=197.0ms max=197.0ms fps=5.0
[PERF] all patterns done, total 7958ms, no hang.
```

| 图案 | 10.2 | 10.3 实测 | Δ |
|---|---|---|---|
| solid | 196.6ms | 196.6ms | 0（符合预期：总线瓶颈） |
| bars | 196.7ms | 196.7ms | 0 |
| gray | 197.0ms | 197.0ms | 0（**字节序试金石通过**：16bit 线序等价成立） |
| checker | 201.1ms | 201.2ms | +0.1（每窗口 2 次位宽切换 ≈0.33µs×300，带内） |
| **KPI：checker−solid** | **4.5ms** | **4.6ms** | +0.1（切换代价，可接受） |

**结论**：PASS。@6.25MHz 数值持平是**预期结果**（本步收益在提频后兑现：
CPU 每像素寄存器写次数 2→1，喂料能力翻倍）。

## 10.4（可选）编译等级实验（计划）

`-Os` → `-O2` 对 bsp_lcd.c 或全工程试一轮，看数值与体积，收益不确定，最后做。

---

## 本阶段不做

| 规格书条目 | 处理 |
|---|---|
| DMA 数据宽度 / burst / FIFO | DMA 尚未引入，等阶段3；宽度届时=半字，直接衔接 10.3 |
| 缓存 | CH32V407 无 D-cache，N/A |
| 总线争用 / FreeRTOS 调度 / 中断优先级 | 无 DMA 无 OS 参与刷新时无从测起，归 DMA/LVGL 阶段 |

## 结果汇总表（随实施更新）

| 步骤 | solid | checker | checker−solid | 窗口软件残余 | 结论 |
|---|---|---|---|---|---|
| 基线 | 196.6ms | 201.9ms | 5.3ms | 3.6µs/窗口 | — |
| 10.1 寄存器化 | 196.6ms | 201.4ms | 4.8ms | 2.0µs | **PASS**（2026-09-24） |
| 10.2 CS 合并 | 196.6ms | 201.1ms | 4.5ms | ≈1.0µs | **PASS**（2026-09-24） |
| 10.3 SPI16 | 196.6ms | 201.2ms | 4.6ms | ≈1.3µs（含切换） | **PASS**（2026-09-24） |

### 阶段10 小结（2026-09-24，10.1~10.3 全部 PASS）

- 窗口开销 17.7 → **15.4 µs/窗口**（软件 3.6 → ≈1.3µs；纯线时间 14.08µs 不可再减）
- **solid 196.6ms = 理论地板，@6.25MHz 轮询方式下软件已无优化空间**——
  剩余 ~1.3µs/窗口软件是 DC 时序必需的 wait_idle + 位宽切换，极限压榨也就
  再省 ~0.2ms/帧，不值得
- 10.1~10.3 的真正价值在**高时钟**：寄存器直访 + 16bit 喂料 + 单 CS 骨架，
  是 12.5/25/50MHz 提频和 DMA 帧传输的前提
- **下一个数量级的收益不在底层**：① SPI 时钟 ×8（196.6→24.6ms 理论）；
  ② DMA（速度不变但 CPU 占用 100%→≈0，FreeRTOS/LVGL 的前提）

## 10.3 补遗：SPI16 扩展到 write_pixels（2026-09-28）

**背景**：LVGL hello-world 联调时发现，10.3 的 16bit 只落在 `bsp_lcd_fill`
（当时性能基准 `test_lcd_perf.c` 只测 fill），**`bsp_lcd_write_pixels` 仍是
逐字节 8bit**——而 LVGL flush_cb 走的恰恰是 write_pixels，LVGL 刷新没吃到
10.3 的收益。

**修改**（单变量，与 fill 同一套 10.3 模式）：`bsp_lcd_write_pixels` 像素流
段 `spi1_datasize_16()` → 循环 `spi1_send_half(pixels[i])` → `lcd_window_end()`
→ `spi1_datasize_8()`。DFF 两处切换点总线均空闲（入口：set_window 的 RAMWR
已 wait_idle+CS↑；出口：end 先 wait_idle），合法。

**验证**：编译 PASS；预期收益同 10.3 结论——@6.25MHz 持平（线时间主导），
高 SPI 时钟（现 /2=100MHz 实验）下 CPU 喂料次数 2→1。**待上板**：跑
PSG_TEST_LVGL_HELLO 确认文字颜色/flush 块数不变（16bit 线序与 8bit×2 等价，
理论上显示应逐像素一致）。
