/*********************************************************************
 * test_bsp_touch.c - 触摸 T1 用例：仅验证"触摸可以用"
 *
 * 对应 Docs/FT6336U 文档 §8 上板顺序的前三步：
 *   1. init 探测地址 0x38（I2C 通不通）
 *   2. FIRMID(0xA6) 读回打印（只作记录，不作身份铁证——文档 §8）
 *   3. 调度器里的打印任务 100ms 轮询：按下流式打印原始坐标，
 *      抬起打一行 release —— 【人工看串口判定的交互用例】
 *
 * runner 打的 [ OK ] 只代表"init 探测 + 任务创建"通过；坐标对不对
 * 要人眼看：打印的是玻璃坐标系原始值（原生竖屏 240x320）：
 *   左上角应约 x=0   y=0
 *   右下角应约 x=239 y=319
 * 把四角 + 中心的实际值记下来 —— T2 填旋转映射表就用这组数。
 * 本用例不做旋转、不接 LVGL（那是 T2）。
 *********************************************************************/
#include "test_config.h"   /* 先 include 再 #if，写反了 = 空文件 = 链接错误 */

#if PSG_TEST_BSP_TOUCH

#include "FreeRTOS.h"
#include "task.h"
#include "test_framework.h"
#include "bsp_touch.h"
#include "bsp_uart.h"

/* 触摸打印任务：100ms 轮询一次（软件 I2C 一笔约 1ms，开销可忽略）。
 * 按下期间持续打印坐标（拖动时能看到坐标连续变化），
 * 抬起只打一行 release，空闲静默——串口不刷屏 */
static void touch_print_task(void *arg)
{
    uint8_t  last_pressed = 0;
    uint32_t err = 0;

    (void)arg;
    for (;;)
    {
        uint16_t x;
        uint16_t y;
        uint8_t  pressed;

        if (bsp_touch_read(&x, &y, &pressed) != 0)
        {
            err++;                          /* I2C 无应答：接线/上拉问题 */
        }
        else if (pressed != 0)
        {
            uart_printf("touch: x=%d y=%d\r\n", (int)x, (int)y);
        }
        else if (last_pressed != 0)
        {
            uart_printf("touch: release (i2c_err=%lu)\r\n", (unsigned long)err);
        }
        last_pressed = pressed;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void test_bsp_touch(void)
{
    uint8_t fw = 0;

    /* init = 复位时序 + 地址探测，约 0.5s（调度器前的阻塞，一次性） */
    TEST_ASSERT(bsp_touch_init() == 0);

    /* FIRMID：读回来记录一下（兼容寄存器，值本身不作判定依据）。
     * 失败时打出失败现场：stage 码表见 bsp_touch.c 头部注释
     * （3=写段NACK 4=读段无数据...；STATER 0x100=地址NACK、
     *  PCFR2 bit14=1=remap已生效、CTLR非0=消息没被受理、
     *  STATR.XDCNT=实际搬运字节数） */
    TEST_ASSERT(bsp_touch_read_regs(FT6336_REG_FIRMID, &fw, 1) == 0);
    {
        uint32_t st = 0, ev = 0, sr = 0, pc = 0, ct = 0, sx = 0;

        bsp_touch_dbg_info(&st, &ev, &sr, &pc, &ct, &sx);
        uart_printf("FT6336 FIRMID=0x%x (dbg: stage=%lu EVR=0x%lx STATER=0x%lx PCFR2=0x%lx CTLR=0x%lx STATR=0x%lx)\r\n",
                    fw, (unsigned long)st, (unsigned long)ev,
                    (unsigned long)sr, (unsigned long)pc,
                    (unsigned long)ct, (unsigned long)sx);
    }
    uart_printf("touch: 请点屏幕四角+中心，记录原始坐标（左上应约(0,0) 右下(239,319)）\r\n");

    /* 坐标流式打印在调度器启动后进行（要 vTaskDelay 控制节奏） */
    TEST_ASSERT(xTaskCreate(touch_print_task, "t_touch", 256, NULL, 2, NULL) == pdPASS);
}

#endif /* PSG_TEST_BSP_TOUCH */
