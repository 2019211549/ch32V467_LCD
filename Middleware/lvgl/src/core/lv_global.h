/**
 * @file lv_global.h
 *
 */

#ifndef LV_GLOBAL_H
#define LV_GLOBAL_H

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/
#include "../lvgl_public.h"

#if LV_USE_DRAW_SW
#include "../draw/sw/lv_draw_sw.h"
#endif
#include "../osal/lv_os_private.h"
#include "../stdlib/builtin/lv_tlsf.h"

#if LV_USE_FONT_COMPRESSED
#include "../font/fmt_txt/lv_font_fmt_txt_private.h"
#endif

#include "../misc/lv_timer_private.h"
#include "../misc/lv_anim_private.h"
#include "../tick/lv_tick_private.h"
#include "../draw/lv_draw_buf_private.h"
#include "../draw/lv_draw_private.h"
#include "../draw/sw/lv_draw_sw_private.h"
#include "../draw/sw/lv_draw_sw_mask_private.h"
#include "../stdlib/builtin/lv_tlsf_private.h"
#include "../debugging/sysmon/lv_sysmon_private.h"
#include "../debugging/test/lv_test_private.h"
#include "../layouts/lv_layout_private.h"

/*********************
 *      DEFINES
 *********************/
#define ZERO_MEM_SENTINEL  0xa1b2c3d4

/**********************
 *      TYPEDEFS
 **********************/

#if LV_USE_SPAN != 0
struct _snippet_stack;
#endif

#if LV_USE_FREETYPE
struct _lv_freetype_context_t;
#endif

#if LV_USE_PROFILER && LV_USE_PROFILER_BUILTIN
struct _lv_profiler_builtin_ctx_t;
#endif

#if LV_USE_NUTTX
struct _lv_nuttx_ctx_t;
#endif

typedef struct _lv_global_t {
    /**
     * User data for the LVGL library. Move from the bottom of the struct
     * to avoid breaking the ABI. E.g., if the user data is used by a
     * closed-source library, this can help to avoid re-compiling the library
     * when the lvgl-related configs are changed.
     */
    void * user_data;   //万能指针 用户自己挂上下文的地方

    bool inited;      //是否已经完成了初始化
    bool deinit_in_progress;     /**< Can be used e.g. in the LV_EVENT_DELETE to deinit the drivers too */  //是否正在去初始化

    lv_ll_t disp_ll;   //所有display的链表
    lv_display_t * disp_refresh;   //当前正在刷新的显示器
    lv_display_t * disp_default;    //当前的默认显示器

    lv_ll_t style_trans_ll;  //样式过渡链表，用于在使用样式动画过渡时的显示
    bool style_refresh;   //样式刷新控制，用于批量刷新的部分，会节省每次更新部分的

    /** Number of widgets that currently have blur or a drop shadow (see
     * lv_obj_t::has_blur). If it is zero lv_obj_invalidate_expand_blur() is skipped. */
    uint32_t blur_obj_cnt;   // 这个变量记录了当前所有显示器上应用了模糊效果的控件总数
    uint32_t style_custom_table_size;  //自定义属性查找表的大小。  //用于拓展UI界面或者图标的属性
    uint32_t style_last_custom_prop_id; //最近注册的自定义属性的 ID。
    uint8_t * style_custom_prop_flag_lookup_table; //即自定义属性标志查找表的地址

    lv_ll_t group_ll; //输入组的全局链表
    lv_group_t * group_default; // 输入设备默认组

    lv_ll_t indev_ll;   //所有的输入设备链表
    lv_indev_t * indev_active;  //当前正在执行的输入设备
    lv_obj_t * indev_obj_active; //当前输入处理上下文中的活动对象。

    uint32_t layout_count;  //布局 ID 的管理计数，子对象怎么摆放
    lv_layout_dsc_t * layout_list;   //所有已注册布局的“描述信息
    bool layout_update_mutex; //布局更新互斥锁 

    uint32_t memory_zero;  //防止越界
    uint32_t math_rand_seed;     //随机种子

    lv_event_t * event_header;  //延时操作的事务
    uint32_t event_last_register_id;  //给回调函数一个ID

    lv_timer_state_t timer_state;  //定时器子系统状态结构体
    lv_anim_state_t anim_state;   //管理所有当前 Animation：
    lv_tick_state_t tick_state;  //管理所有当前 时间：

    lv_draw_buf_handlers_t draw_buf_handlers;  //绘图缓冲区管家  ，通用绘图缓冲区
    lv_draw_buf_handlers_t font_draw_buf_handlers;   //字体专用缓冲区
    lv_draw_buf_handlers_t image_cache_draw_buf_handlers;  /**< Ensure that all assigned draw buffers
                                                            * can be managed by image cache. */ //图像缓存专用缓冲区

    lv_ll_t img_decoder_ll;   //挂着当前系统中所有已注册的图像解码器。
#if LV_USE_OS != LV_OS_NONE
    lv_mutex_t img_decoder_info_lock; //初始化互斥锁，防止多线程竞争
    lv_mutex_t img_decoder_open_lock;
#endif

    lv_cache_t * img_cache; //图像缓存 ，节省cpu
    lv_cache_t * img_header_cache;  //但只缓存图像的元信息

    lv_draw_global_info_t draw_info;  //绘图子系统全局信息
    lv_ll_t draw_sw_blend_handler_ll;  //软件混合处理链表

#if LV_USE_OBSERVER
    lv_ll_t subject_ll;  //主题观察者模式，一个主题变化自动通知所有的观察者
#endif
//当ui控件需要阴影时 开启这个宏，这是ui阴影控件缓存
#if defined(LV_DRAW_SW_SHADOW_CACHE_SIZE) && LV_DRAW_SW_SHADOW_CACHE_SIZE > 0
    lv_draw_sw_shadow_cache_t sw_shadow_cache;
#endif
//复杂渲染绘制开关，圆角阴影啥的
#if LV_DRAW_SW_COMPLEX 
    lv_draw_sw_mask_radius_circle_dsc_arr_t sw_circle_cache;
#endif
 //是否要用到日志功能
#if LV_USE_LOG
    lv_log_print_g_cb_t custom_log_print_cb;
#endif
//记录上一条日志的时间
#if LV_USE_LOG && LV_LOG_USE_TIMESTAMP
    uint32_t log_last_log_time;
#endif

#if LV_USE_THEME_SIMPLE //简单主题
    void * theme_simple;
#endif

#if LV_USE_THEME_DEFAULT // 现代主题
    void * theme_default;
#endif

#if LV_USE_THEME_MONO   //单色主题，只有黑白两个颜色
    void * theme_mono;
#endif

#if LV_USE_STDLIB_MALLOC == LV_STDLIB_BUILTIN  //使用lvgl自己的内存分配方案
    lv_tlsf_state_t tlsf_state;
#endif

    lv_ll_t fsdrv_ll; //文件系统是一个链表
#if LV_USE_FS_STDIO != '\0'
    lv_fs_drv_t stdio_fs_drv;
#endif
#if LV_USE_FS_POSIX
    lv_fs_drv_t posix_fs_drv;
#endif

#if LV_USE_FS_FATFS  //FatFs 库，SD 卡、U 盘常用  //以后可能开启从sd卡中加载图片
    lv_fs_drv_t fatfs_fs_drv;
#endif

#if LV_USE_FS_WIN32 != '\0'
    lv_fs_drv_t win32_fs_drv;
#endif

#if LV_USE_FS_UEFI
    lv_fs_drv_t uefi_fs_drv;
#endif

#if LV_USE_FS_LITTLEFS  //中文字符时可以使用，可以子集化
    lv_fs_drv_t littlefs_fs_drv;
#endif

#if LV_USE_FS_ARDUINO_ESP_LITTLEFS
    lv_fs_drv_t arduino_esp_littlefs_fs_drv;
#endif

#if LV_USE_FS_ARDUINO_SD
    lv_fs_drv_t arduino_sd_fs_drv;
#endif

#if LV_USE_FS_FROGFS
    lv_fs_drv_t frogfs_fs_drv;
#endif

#if LV_USE_FREETYPE
    struct _lv_freetype_context_t * ft_context;
#endif

#if LV_USE_FONT_COMPRESSED
    lv_font_fmt_rle_t font_fmt_rle;
#endif

#if LV_USE_SPAN != 0    //开启富文本功能
    struct _snippet_stack * span_snippet_stack;
#endif

#if LV_USE_PROFILER && LV_USE_PROFILER_BUILTIN  //性能分析器，调试优化瓶颈时需要开启
    struct _lv_profiler_builtin_ctx_t * profiler_context;
#endif


#if LV_USE_FILE_EXPLORER  //文件浏览器控件
    lv_style_t file_explorer_quick_access_style;
    size_t file_explorer_count;
#endif

#if LV_USE_MEM_MONITOR   //发阶段观察内存变化。调试内存泄漏或碎片问题。 调试开发阶段可以开启
    lv_sysmon_backend_data_t sysmon_mem;
#endif

#if LV_USE_IME_PINYIN != 0  //是否开启拼音输入法
    size_t ime_cand_len;
#endif

#if LV_USE_OBJ_ID_BUILTIN //需要序列化 UI 状态。启用对象id系统
    void * objid_array;
    uint32_t objid_count;
#endif

#if LV_USE_TEST  //测试框架，再单元测试 回归测试时可以开启，= 
    lv_test_state_t test_state;
#endif

#if LV_USE_TRANSLATION  //翻译系统
    lv_ll_t translation_packs_ll;
    const char * translation_selected_lang;
#endif

#if LV_USE_NUTTX //NuttX 适配
    struct _lv_nuttx_ctx_t * nuttx_ctx;
#endif

#if LV_USE_OS != LV_OS_NONE  
    lv_mutex_t lv_general_mutex;  //	LVGL在多任务环境下的全局互斥锁
#endif

#if defined(__linux__)  
    lv_linux_proc_stat_t linux_last_proc_stat;
#if LV_SYSMON_PROC_IDLE_AVAILABLE
    uint64_t linux_last_self_proc_time_ticks;
    lv_linux_proc_stat_t linux_last_system_total_ticks_stat;
#endif
#endif

#if LV_USE_OS == LV_OS_FREERTOS
    uint32_t freertos_idle_time_sum;
    uint32_t freertos_non_idle_time_sum;
    uint32_t freertos_task_switch_timestamp;
    bool freertos_idle_task_running;
#endif

#if LV_USE_EVDEV  //EVDEV 输入设备自动发现
    lv_evdev_discovery_t * evdev_discovery;
#endif

#if LV_USE_DRAW_EVE //EVE 外部 GPU 渲染
    lv_draw_eve_unit_t * draw_eve_unit;
#endif
} lv_global_t;

/**********************
 *      MACROS
 **********************/

#if LV_ENABLE_GLOBAL_CUSTOM
#if LV_GLOBAL_USE_CUSTOM_INCLUDE
#include LV_GLOBAL_CUSTOM_INCLUDE
#endif /*LV_GLOBAL_USE_CUSTOM_INCLUDE*/

#ifndef LV_GLOBAL_CUSTOM
#define LV_GLOBAL_CUSTOM() lv_global_default()
#endif
#define LV_GLOBAL_DEFAULT() LV_GLOBAL_CUSTOM()
#else
LV_ATTRIBUTE_EXTERN_DATA extern lv_global_t lv_global;
#define LV_GLOBAL_DEFAULT() (&lv_global)
#endif /*LV_ENABLE_GLOBAL_CUSTOM*/

/**********************
 * GLOBAL PROTOTYPES
 **********************/
#if LV_ENABLE_GLOBAL_CUSTOM
/**
 * Get the default global object for current thread
 * @return  pointer to the default global object
 */
lv_global_t * lv_global_default(void);
#endif
#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*LV_GLOBAL_H*/
