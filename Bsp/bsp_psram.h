/*********************************************************************
 * bsp_psram.h - CH32V467 片上 PSRAM(4MB) 板级驱动
 *
 * 依赖关系：本模块只依赖 Peripheral 库（ch32v4x7_psram/rcc/pwr），
 * 不依赖 RTOS/LVGL —— 调度器启动前（main→test_runner）就能用。
 *
 * 初始化序列【全部来自 WCH 官方 EVT 例程，不要按 RM 手写】：
 *   openwch/ch32v407_ch32v467  EVT/EXAM/PSRAM/PSRAM/User/{main.c,PSRAM.c,PSRAM.h}
 *   （2026-10-15 取回）。寄存器原语 vendored v1.4 库已有（PSRAMInit/
 *   PSRAMSetData/Set_Busy/Set_CMD/Set_MR_ADDR…），但延迟码表和
 *   SetWrLatency/SetRdLatency 的 MR 编程流程 EVT 才有 —— 已原样搬进
 *   bsp_psram.c 并标注出处。
 *
 * 时钟档位选择：PSRAM 时钟跟随 SYSCLK。2026-10-16 板证 400M 档出局
 *   （MR 配置经回读验证全对仍持续读回陈旧拍），主频随之降到 EVT 例程
 *   同款 SYSCLK=300M(HCLK=150M)（system_ch32v4x7.c 预设切换）+ 300M
 *   延迟档（MR4_Write_300M/MR0_Read_300M/Latency_300M，无 MR8）+
 *   die MR 复位（清暖复位残留）。若自检失配，降档次序见 bsp_psram.c
 *   文件头"失败回退梯子"。
 *********************************************************************/
#ifndef BSP_PSRAM_H
#define BSP_PSRAM_H

#include <stdint.h>

/* PSRAM 内存映射数据窗口基地址（EVT 例程 PSRAM_ADDR，AN36001 Link.ld 同款） */
#define BSP_PSRAM_WINDOW_BASE   0x80000000UL
/* CH32V467RET = 32Mb(4MB)，与 Link.ld 的 PSRAM 区域、PSRAM_CAPCFG_32M 一致 */
#define BSP_PSRAM_SIZE          (4UL * 1024UL * 1024UL)

/* 初始化 + 自检。返回 0 = 成功；1 = 初始化后数据自检失配（PSRAM 不可用）。
 * 幂等：重复调用直接返回首次结果（test_bsp_psram 与 bsp_lvgl_port_init
 * 都会调它） */
int bsp_psram_init(void);

/* 首次初始化是否已成功（1=可用 0=不可用/未初始化） */
uint8_t bsp_psram_ready(void);

#endif /* BSP_PSRAM_H */
