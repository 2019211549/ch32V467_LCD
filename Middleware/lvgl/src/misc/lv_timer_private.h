/**
 * @file lv_timer_private.h
 *
 */

#ifndef LV_TIMER_PRIVATE_H
#define LV_TIMER_PRIVATE_H

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/

#include "../lvgl_public.h"

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

/**
 * Descriptor of a lv_timer
 */
struct _lv_timer_t {
#if LV_USE_EXT_DATA
    lv_ext_data_t ext_data;
#endif
    uint32_t period;           /**< How often the timer should run */
    uint32_t last_run;         /**< Last time the timer ran */
    lv_timer_cb_t timer_cb;    /**< Timer function */
    void * user_data;          /**< Custom user data */
    int32_t repeat_count;      /**< 1: One time;  -1 : infinity;  n>0: residual times */
    volatile int paused;
    uint32_t auto_delete : 1;
};

typedef struct {
    lv_ll_t timer_ll;          /**< Linked list to store the lv_timers */ // 所有已创建定时器的链表
 
    bool lv_timer_run;   //定时器系统当前是否正在运 行，防止重入
    uint8_t idle_last;    //连续空闲轮次的计数
    bool timer_deleted;   //在本轮的处理中是否有新的定时器被删除
    bool timer_created;  //在本轮的处理中是否有新的定时器被创建
    volatile uint32_t timer_time_until_next; //距离下一个定时器到期还有多少毫秒。

    bool already_running;        ////already_running 更偏向"本次调用是否嵌套"。
    uint32_t periodic_last_tick; //上一次周期性处理的时间戳。
    uint32_t busy_time; //：定时器处理器累计忙碌的时间
    uint32_t idle_period_start; //本次空闲周期的起始 tick。
    uint32_t run_cnt;  // 定时器处理器被调用的总次数。

    lv_timer_handler_resume_cb_t resume_cb; // 一个回调函数指针，允许用户在被唤醒后进行处理
    void * resume_data; //传给 resume_cb 的用户数据指针。
} lv_timer_state_t;

/**********************
 * GLOBAL PROTOTYPES
 **********************/

/**
 * Init the lv_timer module
 */
void lv_timer_core_init(void);

/**
 * Deinit the lv_timer module
 */
void lv_timer_core_deinit(void);

/**********************
 *      MACROS
 **********************/

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*LV_TIMER_PRIVATE_H*/
