/*********************************************************************
 * bsp_touch.h - FT6336U 电容触摸驱动（I3C 外设 legacy I2C 模式）
 *
 * 硬件：P032B017P-IPS-CTP 的 CTP 部分，FT6336U 自容触摸控制器
 * 协议依据：Docs/FT6336U_触摸控制器_I2C通信协议与接入说明.md
 *   （寄存器映射是 FT6x36 兼容约定，文档明确标注"需实机核验"）
 *
 * 引脚（按接线修改这里）：
 *   PB4  SCL  I3C_SCL_1（GPIO_Remap_I3C 重映射档 1 + AF_PP）
 *   PB5  SDA  I3C_SDA_1（同上）
 *   PA8  INT  输入上拉，T1 轮询不参与，为以后 EXTI 事件化预留
 *   PC4  RST  推挽输出，低有效
 *
 * 传输层：I3C 控制器发 legacy I2C 帧（MTYPE=0100，NOARBH=1 纯 I2C 帧）。
 * 调参点（上板 NACK/丢数据时按序排查）：
 *   1. SDA/SCL 各加 2.2k~4.7k 外部上拉 —— 外设最慢档就有 ~390kHz
 *      （时序字段 8 位 × tHCLK=5ns 封顶），贴着 FT6336 的 400kHz 上限，
 *      上升沿质量全靠上拉
 *   2. 确认 remap 生效（PA15/PB10 等无输出变化、PB4/PB5 有时钟波形）
 *********************************************************************/
#ifndef BSP_TOUCH_H
#define BSP_TOUCH_H

#include <stdint.h>

/* ==================== FT6336 常量（兼容协议，实机核验） ==================== */
#define FT6336_I2C_ADDR     0x38U   /* 7bit 地址：I3C 的 ADD[6:0] 直接填它
                                      （读写方向是 CTLR 里独立的 RNW 位） */

#define FT6336_REG_TD_STATUS 0x02U  /* 触点数量（低 4 位，>2 视为异常） */
#define FT6336_REG_P1_XH     0x03U  /* 触点1：bit3:0=X 高 4 位，bit7:6=事件 */
#define FT6336_REG_P1_XL     0x04U  /* 触点1：X 低 8 位 */
#define FT6336_REG_P1_YH     0x05U  /* 触点1：bit3:0=Y 高 4 位，bit7:4=ID */
#define FT6336_REG_P1_YL     0x06U  /* 触点1：Y 低 8 位 */
#define FT6336_REG_FIRMID    0xA6U  /* 固件版本（只作记录，不作身份铁证） */

/* 初始化：GPIO 配置 + 复位时序（低>=5ms -> 高 -> 等>=300ms，手册 Table 3-5）
 * + 地址 0x38 探测。返回 0 = 芯片有应答；-1 = 总线无应答（查接线/上拉/供电） */
int bsp_touch_init(void);

/* 寄存器连续读：S->0x70->reg->Sr->0x71->N字节->P（末字节 NACK）。
 * 返回 0 成功；-1 参数错或任一步无应答 */
int bsp_touch_read_regs(uint8_t reg, uint8_t *buf, uint8_t len);

/* 单寄存器写（T2 配 G_MODE 中断模式等场合用） */
int bsp_touch_write_reg(uint8_t reg, uint8_t val);

/* bring-up 诊断：读最近一笔失败事务的现场（bsp_touch.c 注释有 stage 码表；
 * 全 0/PCFR2.bit14=1 = 正常；CTLR 非 0 = 消息字没被硬件受理）。参数可传 0 跳过 */
void bsp_touch_dbg_info(uint32_t *stage, uint32_t *evr, uint32_t *stater,
                        uint32_t *pcfr2, uint32_t *ctlr, uint32_t *statr);

/* 读触点 1 状态与坐标（玻璃坐标系，原生竖屏 240x320，未做任何旋转）。
 * pressed: 1=有触点 0=无；x/y: 触点原始坐标。
 * 返回 0 = 读成功（含"无触点"这一正常情况）；-1 = I2C 事务失败 */
int bsp_touch_read(uint16_t *x, uint16_t *y, uint8_t *pressed);

#endif /* BSP_TOUCH_H */
