/*********************************************************************
 * app_main.h - 应用入口：外设初始化 + 创建全部 FreeRTOS 任务
 *
 * 由 User/main.c 调用；main 只管时钟/打印口、测试钩子、启动调度器。
 * 业务代码一律放 App/（按 task_<name>.c 拆分），不要堆进 main.c。
 *********************************************************************/
#ifndef APP_MAIN_H
#define APP_MAIN_H

void app_main(void);

#endif /* APP_MAIN_H */
