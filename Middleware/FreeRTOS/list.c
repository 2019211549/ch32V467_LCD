/*
 * FreeRTOS Kernel V10.4.6
 * Copyright (C) 2021 Amazon.com, Inc. or its affiliates.  All Rights Reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * https://www.FreeRTOS.org
 * https://github.com/FreeRTOS
 *
 */


#include <stdlib.h>

/* Defining MPU_WRAPPERS_INCLUDED_FROM_API_FILE prevents task.h from redefining
 * all the API functions to use the MPU wrappers.  That should only be done when
 * task.h is included from an application file. */
#define MPU_WRAPPERS_INCLUDED_FROM_API_FILE

#include "FreeRTOS.h"
#include "list.h"

/* Lint e9021, e961 and e750 are suppressed as a MISRA exception justified
 * because the MPU ports require MPU_WRAPPERS_INCLUDED_FROM_API_FILE to be
 * defined for the header files above, but not in this file, in order to
 * generate the correct privileged Vs unprivileged linkage and placement. */
#undef MPU_WRAPPERS_INCLUDED_FROM_API_FILE /*lint !e961 !e750 !e9021. */

/*-----------------------------------------------------------
* PUBLIC LIST API documented in list.h
*----------------------------------------------------------*/

/*
 * ============================ 中文学习注释 ============================
 * 【list.c —— FreeRTOS 内核的"地基"文件（全文约 200 行，务必吃透）】
 *
 * 本文件实现侵入式双向环形链表的 5 个基本操作。内核中所有链表操作
 * 最终都会落到这 5 个函数上，理解了它们，tasks.c / queue.c 里的
 * 调度逻辑就成功了一半：
 *
 *   vListInitialise()     初始化链表头（建空表，哨兵自指）
 *   vListInitialiseItem() 初始化节点（标记"不在任何链表上"）
 *   vListInsert()         按 xItemValue 升序【有序】插入（延时表用）
 *   vListInsertEnd()      插到游标 pxIndex 前面【无序】插入（就绪表用）
 *   uxListRemove()        O(1) 摘除任意节点（O(1)! 不用遍历找前驱）
 *
 * 链表是"环形"的：尾部哨兵 xListEnd 的 next 回到头部，遍历时以
 * &xListEnd 作为回到起点的标志。注意 xListEnd 是 MiniListItem_t，
 * 代码里大量出现把它强转成 ListItem_t* 的写法——因为两种结构体的
 * 前 3 个成员（xItemValue/pxNext/pxPrevious）内存布局完全一致。
 * ==================================================================== */

/*********************************************************************
 * 初始化【链表头】—— 建立一条空链表
 *
 * 初始化后的状态（空环形表，哨兵自己咬自己的尾巴）：
 *
 *      pxIndex ──┐
 *                ▼
 *        ┌──> [xListEnd哨兵] ──┐
 *        └─────────────────────┘   (pxNext/pxPrevious 都指向自己)
 *
 * pxIndex 是遍历游标，初始停在哨兵上；uxNumberOfItems = 0。
 *********************************************************************/
void vListInitialise( List_t * const pxList )
{
    /* The list structure contains a list item which is used to mark the
     * end of the list.  To initialise the list the list end is inserted
     * as the only list entry. */
    /* 游标指向尾部哨兵。注意强转：xListEnd 是 MiniListItem_t，
     * 与 ListItem_t 前 3 个成员布局一致，可以安全地当节点用 */
     //首先将让链表使用的游标参数指向尾部的哨兵，刚初始化时链表中没有任何的节点。
    pxList->pxIndex = ( ListItem_t * ) &( pxList->xListEnd ); //前三个相同，所以可以强制转换 /*lint !e826 !e740 !e9087 The mini list structure is used as the list end to save RAM.  This is checked and valid. */

    /* The list end value is the highest possible value in the list to
     * ensure it remains at the end of the list. */
    /* 哨兵的排序值设为最大值 0xFFFFFFFF：升序链表里天然永远垫底，
     * 从此保证哨兵永远处于"尾部"，充当遍历终点标志 */

    //将排序值设为最大值，永远垫底 充当尾部终点
    pxList->xListEnd.xItemValue = portMAX_DELAY;

    /* The list end next and previous pointers point to itself so we know
     * when the list is empty. */
    /* 前后指针都指向自己 => 空表。判断空表就看这个（listLIST_IS_EMPTY）*/

    pxList->xListEnd.pxNext = ( ListItem_t * ) &( pxList->xListEnd );     /*lint !e826 !e740 !e9087 The mini list structure is used as the list end to save RAM.  This is checked and valid. */
    pxList->xListEnd.pxPrevious = ( ListItem_t * ) &( pxList->xListEnd ); /*lint !e826 !e740 !e9087 The mini list structure is used as the list end to save RAM.  This is checked and valid. */

    pxList->uxNumberOfItems = ( UBaseType_t ) 0U; //这是空链表，这是链表节点的计数值为0，说明当前的链表中没有数值

    /* Write known values into the list if
     * configUSE_LIST_DATA_INTEGRITY_CHECK_BYTES is set to 1. */

     //边界检查
    listSET_LIST_INTEGRITY_CHECK_1_VALUE( pxList );
    listSET_LIST_INTEGRITY_CHECK_2_VALUE( pxList );
}
/*-----------------------------------------------------------*/

/*********************************************************************
 * 初始化【链表节点】—— 通常节点内嵌在 TCB/队列里，创建宿主时调用
 * 只做一件事：pxContainer = NULL，表示"我还没挂在任何链表上"。
 * 后面 uxListRemove() 摘除节点后也会把它置回 NULL。
 * 注意：节点的 pxNext/pxPrevious 此时是垃圾值，但不影响——
 * 插入函数会全部重写它们。
 *********************************************************************/
void vListInitialiseItem( ListItem_t * const pxItem )
{
    /* Make sure the list item is not recorded as being on a list. */
    pxItem->pxContainer = NULL;  //只初始化一个链表节点，表示这个节点暂时不属于任何链表，只需要先初始化这个就行，

    /* Write known values into the list item if
     * configUSE_LIST_DATA_INTEGRITY_CHECK_BYTES is set to 1. */
    listSET_FIRST_LIST_ITEM_INTEGRITY_CHECK_VALUE( pxItem );
    listSET_SECOND_LIST_ITEM_INTEGRITY_CHECK_VALUE( pxItem );
}
/*-----------------------------------------------------------*/

/*********************************************************************
 * 【无序】插入：把新节点插到游标 pxIndex 的【前面】
 *
 * 名字叫 "InsertEnd"，但插入点不是链表物理尾部，而是"逻辑尾部"——
 * 游标 pxIndex 的紧前面。这有个精妙的效果：
 *
 * 调度器用 listGET_OWNER_OF_NEXT_ENTRY() 轮流取节点时，游标会绕环
 * 前进。新来的任务插在游标前面，意味着它是"最后一个被轮到的"——
 * 已经在表里的老任务不会被它插队，这保证了同优先级任务间的公平轮转。
 *
 * 典型用户：就绪链表 pxReadyTasksLists[]（任务变成就绪时插入）。
 * 对比：vListInsert() 是有序插入，用于延时链表（按唤醒时刻排序）。
 *
 * 指针操作图解（把 N 插到 I 前面，P 是 I 原来的前驱）：
 *      改之前:  ... <-> P <-> I <-> ...
 *      改之后:  ... <-> P <-> N <-> I <-> ...
 * 四步换指针（教科书式双向链表插入）。
 *********************************************************************/
void vListInsertEnd( List_t * const pxList,
                     ListItem_t * const pxNewListItem )
{
    ListItem_t * const pxIndex = pxList->pxIndex;

    /* Only effective when configASSERT() is also defined, these tests may catch
     * the list data structures being overwritten in memory.  They will not catch
     * data errors caused by incorrect configuration or use of FreeRTOS. */
    listTEST_LIST_INTEGRITY( pxList );
    listTEST_LIST_ITEM_INTEGRITY( pxNewListItem );

    /* Insert a new list item into pxList, but rather than sort the list,
     * makes the new list item the last item to be removed by a call to
     * listGET_OWNER_OF_NEXT_ENTRY(). */
    /* 步骤1&2：新节点 N 先把两条指针指向邻居（此时环还没接上，P 还不知道 N）*/
    pxNewListItem->pxNext = pxIndex;
    pxNewListItem->pxPrevious = pxIndex->pxPrevious;

    /* Only used during decision coverage testing. */
    mtCOVERAGE_TEST_DELAY();

    /* 步骤3&4：让邻居 P 和 I 回指 N，环闭合 */
    pxIndex->pxPrevious->pxNext = pxNewListItem;
    pxIndex->pxPrevious = pxNewListItem;

    /* Remember which list the item is in. */
    pxNewListItem->pxContainer = pxList;

    ( pxList->uxNumberOfItems )++;
}
/*-----------------------------------------------------------*/

/*********************************************************************
 * 【有序】插入：按 xItemValue 升序找到位置后插入
 *
 * 典型用户：
 *   - 延时链表 xDelayedTaskList：xItemValue = 唤醒时刻(tick)，
 *     于是链表头部永远是"最先该醒"的任务，tick 中断只需检查表头，
 *     不用遍历全表 —— 这就是 FreeRTOS 高效延时的秘密。
 *   - 事件链表：xItemValue = (portMAX_PRIORITY - 任务优先级)，
 *     优先级最高的任务数值最小排最前，唤醒等待者时从头取即最高优先级。
 *
 * 同值规则：新节点插在同值节点【之后】。就绪表里所有任务 xItemValue
 * 相同，先后插入的任务按顺序排队，保证同等条件下先来先被调度。
 *********************************************************************/
void vListInsert( List_t * const pxList,
                  ListItem_t * const pxNewListItem )
{
    //就是一个临时游标，临时节点
    ListItem_t * pxIterator;
    //睡醒值
    const TickType_t xValueOfInsertion = pxNewListItem->xItemValue;

    /* Only effective when configASSERT() is also defined, these tests may catch
     * the list data structures being overwritten in memory.  They will not catch
     * data errors caused by incorrect configuration or use of FreeRTOS. */
     //完整性比较
    listTEST_LIST_INTEGRITY( pxList );
    listTEST_LIST_ITEM_INTEGRITY( pxNewListItem );

    /* Insert the new list item into the list, sorted in xItemValue order.
     *
     * If the list already contains a list item with the same item value then the
     * new list item should be placed after it.  This ensures that TCBs which are
     * stored in ready lists (all of which have the same xItemValue value) get a
     * share of the CPU.  However, if the xItemValue is the same as the back marker
     * the iteration loop below will not end.  Therefore the value is checked
     * first, and the algorithm slightly modified if necessary. */
    /* 特例：插入值 == portMAX_DELAY（哨兵的值）。若走下面的 for 循环，
     * 条件 "pxNext->xItemValue <= xValueOfInsertion" 对哨兵也成立，
     * 循环永远停不下来（绕环一圈回到哨兵仍满足），死循环！
     * 所以直接短路：插到"最后一个真实节点"后面（= 哨兵前面） */

    if( xValueOfInsertion == portMAX_DELAY )
    {
        pxIterator = pxList->xListEnd.pxPrevious;  //排在哨兵节点中的上一个
    }
    else
    {
        /* *** NOTE ***********************************************************
        *  If you find your application is crashing here then likely causes are
        *  listed below.  In addition see https://www.FreeRTOS.org/FAQHelp.html for
        *  more tips, and ensure configASSERT() is defined!
        *  https://www.FreeRTOS.org/a00110.html#configASSERT
        *
        *   1) Stack overflow -
        *      see https://www.FreeRTOS.org/Stacks-and-stack-overflow-checking.html
        *   2) Incorrect interrupt priority assignment, especially on Cortex-M
        *      parts where numerically high priority values denote low actual
        *      interrupt priorities, which can seem counter intuitive.  See
        *      https://www.FreeRTOS.org/RTOS-Cortex-M3-M4.html and the definition
        *      of configMAX_SYSCALL_INTERRUPT_PRIORITY on
        *      https://www.FreeRTOS.org/a00110.html
        *   3) Calling an API function from within a critical section or when
        *      the scheduler is suspended, or calling an API function that does
        *      not end in "FromISR" from an interrupt.
        *   4) Using a queue or semaphore before it has been initialised or
        *      before the scheduler has been started (are interrupts firing
        *      before vTaskStartScheduler() has been called?).
        *   5) If the FreeRTOS port supports interrupt nesting then ensure that
        *      the priority of the tick interrupt is at or below
        *      configMAX_SYSCALL_INTERRUPT_PRIORITY.
        **********************************************************************/

        /* 从哨兵出发，只要"下一个节点的值还 <= 待插值"就继续前进。
         * 循环停下来时，pxIterator 正好是"最后一个值 <= 待插值"的节点，
         * 新节点就该插在它后面（同值插后面的规则也是这么来的）。
         * 空循环体——这个 for 的全部意义就是移动 pxIterator 本身 */
        for( pxIterator = ( ListItem_t * ) &( pxList->xListEnd ); pxIterator->pxNext->xItemValue <= xValueOfInsertion; pxIterator = pxIterator->pxNext ) /*lint !e826 !e740 !e9087 The mini list structure is used as the list end to save RAM.  This is checked and valid. *//*lint !e440 The iterator moves to a different value, not xValueOfInsertion. */
        {
            /* There is nothing to do here, just iterating to the wanted
             * insertion position. */
        }
    }

    /* 标准 doubly-linked 插入四步：N 前后先指好，再让两个邻居回指 N。
     * 插入位置在 pxIterator 与 pxIterator->pxNext 之间 */
    pxNewListItem->pxNext = pxIterator->pxNext;
    pxNewListItem->pxNext->pxPrevious = pxNewListItem;
    pxNewListItem->pxPrevious = pxIterator;
    pxIterator->pxNext = pxNewListItem;

    /* Remember which list the item is in.  This allows fast removal of the
     * item later. */
    pxNewListItem->pxContainer = pxList;

    ( pxList->uxNumberOfItems )++;
}
/*-----------------------------------------------------------*/

/*********************************************************************
 * 摘除节点 —— O(1)，这是侵入式链表最大的优点
 *
 * 普通链表删节点要先遍历找前驱 O(n)；这里每个节点自己记录了
 * 前驱(pxPrevious)、后继(pxNext)、所在链表(pxContainer)，两步
 * 指针互换就把节点从环里"拆"下来了，跟链表有多长没关系。
 *
 * 返回值：摘除后该链表剩余节点数。调用方（tasks.c）用它判断
 * "这条链表是否空了"来决定要不要做额外清理（比如切换延时表）。
 *
 * 典型场景：任务从"就绪表"搬到"延时表" = uxListRemove + vListInsert
 *********************************************************************/
UBaseType_t uxListRemove( ListItem_t * const pxItemToRemove )
{
/* The list item knows which list it is in.  Obtain the list from the list
 * item. */
    /* 从节点反查它挂在哪条链表上（侵入式的红利）*/
    //先通过这个链表中的节点 反查它挂在哪个链表上，主要 要将它从这个链表中去除
    List_t * const pxList = pxItemToRemove->pxContainer;

    /* 让我的前驱和后继互相牵手，把自己从环里跳过去。
     * 注意顺序无所谓，两行执行完，环就闭合了，但节点自身的指针
     * 还指向旧邻居（悬空指针）——不过没关系，下次插入时会被覆盖 */

    pxItemToRemove->pxNext->pxPrevious = pxItemToRemove->pxPrevious;
    pxItemToRemove->pxPrevious->pxNext = pxItemToRemove->pxNext;

    /* Only used during decision coverage testing. */
    mtCOVERAGE_TEST_DELAY();

    /* Make sure the index is left pointing to a valid item. */
    /* 细节：如果游标正好指着被删的节点，就把游标退到前一个。
     * 否则游标会悬空指向一个已经不在链表里的节点，
     * listGET_OWNER_OF_NEXT_ENTRY() 轮转调度就会漏号/崩溃。
     * 退到前驱还保证了"下一个被轮到的"是原来排在它后面的任务，公平性不变 */
    if( pxList->pxIndex == pxItemToRemove )
    {
        pxList->pxIndex = pxItemToRemove->pxPrevious;
    }
    else
    {
        mtCOVERAGE_TEST_MARKER();
    }

    pxItemToRemove->pxContainer = NULL;
    ( pxList->uxNumberOfItems )--;

    return pxList->uxNumberOfItems;
}
/*-----------------------------------------------------------*/
