/**
 * @file lv_draw_buf_private.h
 *
 */

#ifndef LV_DRAW_BUF_PRIVATE_H
#define LV_DRAW_BUF_PRIVATE_H

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

struct _lv_draw_buf_handlers_t {
    lv_draw_buf_malloc_cb_t buf_malloc_cb;   // 怎么分配
    lv_draw_buf_free_cb_t buf_free_cb;      //怎么释放
    lv_draw_buf_copy_cb_t buf_copy_cb;          //怎么复制
    lv_draw_buf_align_cb_t align_pointer_cb;        //怎么对齐
    lv_draw_buf_cache_operation_cb_t invalidate_cache_cb; //无效化缓存
    lv_draw_buf_cache_operation_cb_t flush_cache_cb;         //刷新缓存
    lv_draw_buf_width_to_stride_cb_t width_to_stride_cb; //一行占多少字节
    lv_draw_buf_clear_cb_t buf_clear_cb;    //清除
};

/**********************
 * GLOBAL PROTOTYPES
 **********************/

/**
 * Called internally to initialize the draw_buf_handlers in lv_global
 */
void lv_draw_buf_init_handlers(void);

/**
 * TODO(v10): the draw buffer should describe its own storage well enough that the
 *            layer isn't needed. Merge this back into `lv_draw_buf_clear()` then.
 *
 * @param draw_buf          pointer to draw buffer
 * @param a                 the area to clear @nullable. When NULL the whole buffer is cleared
 * @param layer             the layer `draw_buf` belongs to @nullable
 */
void lv_draw_buf_clear_ex(lv_draw_buf_t * draw_buf, const lv_area_t * a, lv_layer_t * layer);


/**********************
 *      MACROS
 **********************/

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*LV_DRAW_BUF_PRIVATE_H*/
