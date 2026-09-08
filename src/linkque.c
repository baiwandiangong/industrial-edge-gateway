/*
 * linkque.c - 链式 FIFO 队列实现
 *
 * ===== 实现细节 =====
 *
 * 入队（EnterLinkQue）：
 *   1. malloc 一个新节点
 *   2. memcpy 把数据复制到节点中（深拷贝，不是指针赋值）
 *   3. 如果队列为空，head = tail = 新节点
 *   4. 否则，tail->next = 新节点，tail = 新节点
 *   5. clen++
 *
 * 出队（QuitLinkQue）：
 *   1. 保存 head 节点指针
 *   2. head 后移一位
 *   3. 如果 head 变成 NULL，tail 也要置 NULL（队列空了）
 *   4. free 原 head 节点
 *   5. clen--
 *
 * 为什么入队/出队分开而不是一个函数？
 *   有些场景需要"查看队首但不移除"（peek），分开更灵活。
 *   mailbox 的 recv_msg 就是 peek + 判断 → 再出队。
 */

#include "linkque.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LinkQue *CreateLinkQue(void)
{
    LinkQue *lq = (LinkQue *)malloc(sizeof(LinkQue));
    if (NULL == lq)
    {
        printf("CreateLinkQue malloc error\n");
        return NULL;
    }
    lq->head = NULL;
    lq->tail = NULL;
    lq->clen = 0;
    return lq;
}

int EnterLinkQue(LinkQue *lq, DATATYPE *newdata)
{
    if (NULL == lq || NULL == newdata)
    {
        printf("EnterLinkQue: invalid parameter\n");
        return 1;
    }

    /* 分配新节点，注意：data 字段是嵌入在节点中的，不是独立 malloc */
    LinkQueNode *newnode = (LinkQueNode *)malloc(sizeof(LinkQueNode));
    if (NULL == newnode)
    {
        printf("EnterLinkQue malloc error\n");
        return 1;
    }

    /* 深拷贝数据，确保出队后发送方释放原数据不影响队列 */
    memcpy(&newnode->data, newdata, sizeof(DATATYPE));
    newnode->next = NULL;

    if (IsEmptyLinkQue(lq))
    {
        /* 空队列：新节点既是头也是尾 */
        lq->head = newnode;
        lq->tail = newnode;
    }
    else
    {
        /* 尾插法：挂在 tail 后面 */
        lq->tail->next = newnode;
        lq->tail = newnode;
    }
    lq->clen++;
    return 0;
}

int GetSizeLinkQue(LinkQue *lq)
{
    if (NULL == lq)
        return 0;
    return lq->clen;
}

int IsEmptyLinkQue(LinkQue *lq)
{
    if (NULL == lq)
        return 1;
    return 0 == lq->clen;
}

int DestroyLinkQue(LinkQue *lq)
{
    if (NULL == lq)
        return 1;

    /* 逐个出队释放所有节点 */
    int size = GetSizeLinkQue(lq);
    int i = 0;
    for (i = 0; i < size; i++)
    {
        QuitLinkQue(lq);
    }

    free(lq);
    return 0;
}

DATATYPE *GetHeadLinkQue(LinkQue *lq)
{
    if (NULL == lq || IsEmptyLinkQue(lq))
    {
        return NULL;
    }
    /* 返回队首数据指针（只读，不删除） */
    return &lq->head->data;
}

int QuitLinkQue(LinkQue *lq)
{
    if (NULL == lq || IsEmptyLinkQue(lq))
    {
        return 1;
    }

    LinkQueNode *tmp = lq->head;      // 保存旧头节点
    lq->head = lq->head->next;        // head 后移

    if (NULL == lq->head)
    {
        lq->tail = NULL;              // 队列空了，tail 也要置空
    }

    free(tmp);                         // 释放旧头节点
    lq->clen--;
    return 0;
}
