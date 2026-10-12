/*********************************************************************
 * test_sanity.c - 框架自检（也是新测试文件的模板）
 *
 * 新建测试文件照抄本文件的两层结构：
 *   #include "psg_config.h" + "test_config.h"
 *   #if PSG_TEST_<MODULE>   （非单元测试模式下自动为 0）
 *********************************************************************/
#include "psg_config.h"
#include "test_config.h"

#if PSG_TEST_SANITY

#include "test_framework.h"

void test_sanity(void)
{
    /* 整数断言 */
    TEST_ASSERT(1);
    TEST_ASSERT_EQ_INT(2 + 3, 5);

    /* 浮点近似断言（软浮点：只比较，不打印 %f） */
    TEST_ASSERT_NEAR(0.1 + 0.2, 0.3, 1e-9);

    /* 需要验证 FAIL 路径打印时，放开下面这条反例：
     * TEST_ASSERT_EQ_INT(1, 2); */
}

#endif /* PSG_TEST_SANITY */
