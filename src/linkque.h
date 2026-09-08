/*
 * linkque.h - 链式 FIFO 队列
 *
 * ===== 设计思路 =====
 *
 * 为什么用链式队列而不是数组/环形缓冲区？
 *   1. 动态大小：线程间消息数量不定，链表按需分配，不会溢出
 *   2. 操作简单：先进先出只需要 head 出队、tail 入队
 *   3. 无数据搬移：ArrayDeque 出队需要搬移元素，链表只需改指针
 *
 * 数据类型解耦：
 *   通过 typedef MAIL_DATA DATATYPE，队列本身不关心存的是什么数据。
 *   如果想改消息格式，只需改 typedef 一行代码。
 *
 * 非线程安全：
 *   队列本身不加锁，由上层的 mailbox 统一管理互斥。
 *   这样设计是故意的——锁的粒度和策略由调用方决定，队列只做一件事。
 */

#ifndef _LINKQUE_H_
#define _LINKQUE_H_

#include <pthread.h>

/* ===== 数据类型定义 =====
 * 这里放的是 mailbox 系统的消息结构体。
 * 为什么放在 linkque 的头文件里？
 * - 因为队列的 DATATYPE 就是 MAIL_DATA
 * - 队列的上层是 mailbox，mailbox 需要知道消息格式
 * - 放在这里是最小依赖原则：队列的头文件自己就能说明白"我存的是什么"
 */

typedef struct mail_data
{
    pthread_t id_of_sender;      // 发送者线程 ID（内核自动分配，唯一）
    char name_of_sender[256];    // 发送者名称（注册时指定的名字）
    pthread_t id_of_recver;      // 接收者线程 ID
    char name_of_recver[256];    // 接收者名称
    int payload_len;             // 有效数据长度（字节）
    char data[512];              // 有效数据载荷（最大 512 字节）
} MAIL_DATA;

/* 队列存储的数据类型 = MAIL_DATA */
typedef MAIL_DATA DATATYPE;

/* 队列节点结构体 */
typedef struct quenode
{
    DATATYPE data;           // 节点中直接存储数据（而非指针），减少一次内存分配
    struct quenode *next;    // 指向下一个节点
} LinkQueNode;

/* 队列控制结构体：维护头尾指针和长度计数 */
typedef struct _linkque
{
    LinkQueNode *head;   // 出队端（dequeue）
    LinkQueNode *tail;   // 入队端（enqueue）
    int clen;            // 当前队列长度（方便 isEmpty 检查）
} LinkQue;

/* ===== 接口说明 =====
 * CreateLinkQue   - 创建空队列（malloc 分配）
 * EnterLinkQue    - 入队：把数据 COPY 到新节点并追加到尾部
 * GetHeadLinkQue  - 读队首：返回队首数据指针（不出队）
 * QuitLinkQue     - 出队：释放队首节点
 * GetSizeLinkQue  - 获取队列当前长度
 * IsEmptyLinkQue  - 判断队列是否为空
 * DestroyLinkQue  - 销毁队列（释放所有节点 + 控制结构体）
 */

LinkQue *CreateLinkQue(void);
int EnterLinkQue(LinkQue *lq, DATATYPE *newdata);
DATATYPE *GetHeadLinkQue(LinkQue *lq);
int QuitLinkQue(LinkQue *lq);
int GetSizeLinkQue(LinkQue *lq);
int IsEmptyLinkQue(LinkQue *lq);
int DestroyLinkQue(LinkQue *lq);

#endif
