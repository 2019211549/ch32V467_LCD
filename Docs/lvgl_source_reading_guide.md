# LVGL 9.6 源码学习路线图

> 针对本项目（PSG 固件，CH32V407RET）定制。
> 前置条件：已吃透 FreeRTOS 源码 + 已跑通 hello lvgl 移植（`Bsp/bsp_lvgl_port.c` + `Test/test_lvgl_hello.c`）。
> 所有行数为 2026-09-28 实测，源码位于 `Middleware/lvgl/src/`。
> 公共头文件在 `Middleware/lvgl/include/lvgl/`（9.6 新布局），应用代码只 include `"lvgl.h"`。

---

## 0. 读码总原则

1. **顺着你跑通的调用链读**（hello lvgl 那 5 步），不要从 `lv_obj.c` 第 1 行硬啃——2322 行会当场劝退
2. **用 FreeRTOS 类比迁移**——你已吃透 RTOS，LVGL 至少 4 块可以对照着读（见下表），学习成本减半
3. **每个文件先读"结构体定义 + 文件头注释"，再读函数**——LVGL 的数据结构就是它的骨架
4. **配合 GDB 单步**比干读高效 10 倍：在 `flush_cb`、`lv_refr_now`、`lv_timer_handler` 打断点，看真实数据流动（调试配方见 CLAUDE.md / 记忆：openocd + wch-riscv.cfg + gdb）
5. 已从构建排除的目录（drivers、libs/\*、11 个 draw GPU 后端、debugging/monkey+profiler…）**直接跳过**，读了也编不进固件

### FreeRTOS ↔ LVGL 类比速查表（学习的最大杠杆）

| 你已掌握的 FreeRTOS | 对应 LVGL | 相同的思想 | 关键差异 |
|---|---|---|---|
| `task.c` 调度器 + 就绪链表 | `lv_timer.c` + `lv_timer_handler()` | 链表轮询、按到期时间排序 | LVGL 是**协作式单线程**：`lv_timer_handler` 是在**你的任务里**跑的迷你调度器，不抢占比 |
| `heap_4.c`（first-fit + 合并） | `lv_tlsf.c`（TLSF，32K 池） | 静态数组内存池 + 碎片管理 | TLSF 用两级位图做到 O(1) 分配，牺牲少量 RAM 换确定性 |
| `xTaskGetTickCount()` | `lv_tick.c` | 全局毫秒时基 | LVGL 的 tick 是**喂**进去的（`lv_tick_set_cb`），不自己管硬件定时器 |
| 队列 / 任务通知 | `lv_event.c` | 解耦收发 | 事件是**同步冒泡**的（子→父向上传播），不是异步排队 |
| `port.c` 移植层 | `osal/` + `lv_conf.h` | 隔离平台差异 | LVGL 默认 `LV_OS_NONE`（无锁空实现），单任务下零开销 |

---

## 阶段 1：渲染主链路（hello lvgl 走过的路）—— 约 1 周

**读法：按调用顺序，不按目录顺序。** 每个文件标注了"它回答什么问题"和"跳过什么"。

| # | 文件 | 行数 | 回答的问题 | 读什么 / 跳过什么 |
|---|---|---|---|---|
| 1 | `Bsp/bsp_lvgl_port.c`（自己写的） | ~90 | 移植层边界在哪 | 起点。复习 flush_cb 的 area/px_map 从哪来、到哪去 |
| 2 | `lv_init.c` | 541 | `lv_init()` 到底初始化了什么 | 只追 `lv_init()` 主线：全局状态、默认 font/theme/timer 的注册。跳过各种 subsystem 的 `xxx_init` 细节 |
| 3 | `display/lv_display.c` | 1766 | 显示屏的抽象是什么 | 只读 4 个：`lv_display_create` / `set_buffers` / `flush_ready` / `lv_display_render_mode_t` 枚举注释。**PARTIAL/DIRECT/FULL 三种模式**的注释是必读精华 |
| 4 | `core/lv_obj_class.c` | 243 | **控件的本质**：class（模板）+ instance（实例） | 全读。`lv_obj_class_init_obj()` 看"一个控件出生时发生了什么"。这是理解所有 widget 的钥匙 |
| 5 | `widgets/label/lv_label.c` | 1588 | 我用的 label 内部长什么样 | 只读 3 个函数：`lv_label_create` / `lv_label_set_text`（注意字符串复制进 32K 池）/ `lv_label_draw`。其余当字典 |
| 6 | `core/lv_obj_style.c` | 1729 | `set_style_text_color` 一行代码去了哪 | **只追一条线**：set → 级联缓存失效 → 渲染时 `lv_obj_get_style_*` 查询。样式是 LVGL 最绕的子系统，第一遍严禁深入 |
| 7 | `core/lv_refr.c` | 1556 | **整个渲染的心脏**：脏区 → 分块 → 渲染 → flush | 全文档最重要的一章。精读 `lv_display_refr_timer()`：区域合并、按画布切 32 行块、调 draw unit、等 flush_ready。读完你对 PARTIAL 模式的理解就闭环了 |
| 8 | `draw/sw/lv_draw_sw_letter.c` | 406 | 一个字符怎么变成画布上的像素 | 全读。406 行不多：取 glyph 点阵 → 4bit alpha 逐像素与背景混色。**承接字库讨论**，读完字体渲染全通 |
| 9 | `font/lv_font.c` + `font/fmt_txt/lv_font_fmt_txt.c` | 271 + 749 | 字体数据结构怎么查表 | `lv_font.c` 全读；`fmt_txt` 只看 3 个结构体（`lv_font_t` / glyph_dsc / cmap），函数跳过 |

**阶段 1 自测**（能口头回答即过关）：
- flush_cb 的 `area` 参数是怎么算出来的？为什么竖屏每块最多 32 行？
- "Hello LVGL!" 这 11 个字节，从字符串到屏幕红色像素，经过哪几个内存区域？（提示：flash 常量 → 32K 池 → 15K 画布 → SPI）
- `lv_display_flush_ready()` 不调会怎样？（答案：渲染卡死等不到下一块）

---

## 阶段 2：LVGL 的"内核"（FreeRTOS 对照迁移）—— 约 1 周

| # | 文件 | 行数 | 回答的问题 | FreeRTOS 对照 |
|---|---|---|---|---|
| 1 | `tick/lv_tick.c` | 112 | 时基从哪来 | 最短，先读建立信心。对照：为什么以后 task_ui 必须挂 `lv_tick_set_cb`（`lv_timer_handler` 靠 tick 差值判断哪个定时器到期） |
| 2 | `misc/lv_timer.c` | 416 | **迷你调度器**怎么转 | 精读 `lv_timer_handler()`：遍历链表、算 elaps、跑到期回调、算下次唤醒。对照 `taskSELECT_HIGHEST_PRIORITY_TASK`——同样的"就绪队列"思想，单线程版 |
| 3 | `misc/lv_anim.c` | 887 | 动画的本质 | 动画 = 专用的 timer + 每帧插值回调（`anim_exec_cb`）。读 `lv_anim_timer`。写 UI 后必有动画，先懂原理 |
| 4 | `stdlib/builtin/lv_tlsf.c` | 1250 | 32K 池怎么管理 | **与 heap_4.c 对照读，收获最大**：heap_4 = first-fit + 相邻合并；TLSF = 两级隔离链表 + 位图 O(1)。看 `lv_tlsf_malloc` 一条主线即可 |
| 5 | `misc/lv_event.c` + `core/lv_obj_event.c` | 520 + 485 | 事件怎么冒泡 | 发送→捕获→冒泡三阶段；`lv_obj_add_event_cb` 的回调存在哪。对照：这是"同步版的任务通知" |
| 6 | `osal/lv_os_none.c` + `osal/lv_os.h` | 51 + ~100 | OSAL 空实现的意义 | 全读（很短）。所有锁/线程原语宏展开为空——**单任务使用 LVGL 的零开销证明**。以后开 `LV_OS_FREERTOS` 前后各读一遍 `lv_freertos.c`，对比差异 |

**阶段 2 自测**：
- `lv_timer_handler()` 为什么必须被周期调用？它和 FreeRTOS 调度器最本质的区别是什么？
- 不开 `LV_OS_FREERTOS`、多任务同时调 LVGL API 会发生什么？（答案：数据竞争，无锁保护——所以官方约定所有 LVGL 调用收敛到一个任务）
- TLSF 相比 heap_4 拿什么换 O(1)？（答案：每个池固定两级位图 + 空闲块分级链表的空间开销）

---

## 阶段 3：控件 / 布局 / 主题（写 UI 前扫读）—— 约 3 天

| # | 文件 | 行数 | 读法 |
|---|---|---|---|
| 1 | `core/lv_obj_tree.c` | 855 | 控件树的增删遍历。读 `lv_obj_create`/`delete`/`screen_load`——理解"界面切换"的本质是换树根 |
| 2 | `core/lv_obj_pos.c` | 1769 | **不要通读**。只追一次布局计算流程：invalidate → `lv_obj_refr_size` → 布局更新。坐标系统（相对父对象）看文件头注释 |
| 3 | `layouts/flex/lv_flex.c` | 676 | 弹性布局，写 UI 最常用。配合 `lv_obj_set_flex_flow` 等 API 注释读 |
| 4 | `themes/default/lv_theme_default.c` | 1264 | 扫读：主题如何在控件创建时自动套样式（这就是 hello lvgl 白底的来源）。知道结构即可 |
| 5 | `widgets/button/lv_button.c` | 73 | **最短控件范本**，73 行看完一个控件的完整生命周期（class 定义 + 事件响应 + 状态样式）。之后任何控件都是这个模式的放大 |
| 6 | `core/lv_obj.c` | 2322 | **当字典查，永远不通读**。需要时按函数查 |

**阶段 3 自测**：
- `lv_obj_center()` 之后坐标什么时候真正算出来？（答案：不是当场，是下次刷新前统一布局更新——"延迟计算"思想）
- 新建一个控件不指定父对象会怎样？screen 是什么？

---

## 阶段 4：按需再读（现在不读，用到再回来查）

| 文件 | 行数 | 什么时候读 |
|---|---|---|
| `indev/lv_indev.c` | 2045 | **接触摸屏前**（FT6336U 移植时）：输入设备抽象、按下/移动/抬起的状态机、为什么 read_cb 也要周期被调 |
| `draw/lv_draw_buf.c` | 752 | 优化刷新时（DMA、双缓冲、换画布尺寸） |
| `image/lv_image_decoder.c` + `misc/cache/` | ~1000 | 显示图片/波形位图时 |
| `core/lv_global.h` | 307 | 读主线时迷路了就回来翻：所有全局状态的唯一容器，看它就知道 LVGL 运行时"家底"有哪些 |
| `draw/lv_draw.c` | 839 | 深入渲染管线时（draw task 派发给 draw unit 的机制） |
| `draw/sw/lv_draw_sw.c` | 518 | 同上 |

---

## 明确不读清单（省下的时间就是赚到的）

- `draw/` 下 11 个 GPU 后端目录（dma2d/nxp/vg_lite/…）——已排除构建，本 MCU 无 GPU
- `libs/` 全部（bar code/freetype/gif/png…）——已排除构建，用到哪个再开哪个
- `font/lv_font_montserrat_*.c` 等字库的**位图数据部分**——是数据不是代码，只看文件头的生成命令和最底部的 `lv_font_t` 描述符
- `widgets/` 里暂时用不到的控件（calendar/chart/menu…）——控件读法在阶段 3 #5 已统一，用到哪个读哪个
- `debugging/`、`drivers/`——已排除构建
- `include/lvgl/` 下的 API 头文件——不是"读"的对象，是**查**的对象（配合 IDE 跳转）

---

## 推荐学习节奏（与项目里程碑绑定）

| 时间 | 阅读 | 同步做什么 |
|---|---|---|
| 第 1 周 | 阶段 1 渲染主链路 | GDB 跟一遍 hello lvgl；试着改画布行数（320×24 → 320×48）观察 flush 块数变化 |
| 第 2 周 | 阶段 2 内核层 | 动手写 `App/task_ui.c`：`lv_tick_set_cb` + `lv_timer_handler` 常驻循环（tick 问题在实战中理解） |
| 第 3 周 | 阶段 3 控件布局 | 搭 PSG 界面静态框架（label/图表/按钮），顺带做中文字体（lv_font_conv） |
| 之后 | 阶段 4 按需 | 触摸（indev）、DMA 刷新（draw_buf）、波形显示（image） |

> 总量：阶段 1+2 精读约 **6800 行**，阶段 3 扫读约 **4600 行**——比你当年读 FreeRTOS 内核（~10000 行）还少。
