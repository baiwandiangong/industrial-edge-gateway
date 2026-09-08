/*
 * mailbox.h - 线程间消息传递系统（Actor 模型雏形）
 *
 * ===== 设计思路 =====
 *
 * 为什么要自己造一个消息中间件而不是直接用条件变量？
 *   1. 解耦：发送方不需要知道接收方的内部状态，只需要知道名字
 *   2. 异步：发送方放完消息就继续工作，不会被接收方的处理速度阻塞
 *   3. 可扩展：新增一个线程只需 register + while(1){recv_msg}，不需改已有代码
 *   4. 可调试：所有线程间通信都经过 mailbox，加个 log 就能追踪全部消息流
 *
 * 通信流程：
 *   [发送方] → send_msg(mbs, "目标线程名", &mail_data) → [目标线程的 LinkQue]
 *   [接收方] → recv_msg(mbs, &mail_data)             ← [自己的 LinkQue]
 *
 * 数据结构：
 *   MBS (MailBox System) 是整个系统的"邮局"：
 *   - head: 链表头，所有注册的线程节点挂在这个链表上
 *   - mutex: 保护链表中所有队列的操作
 *
 *   LIST_DATA 是"线程信箱"节点：
 *   - tid: 线程 ID（内核分配）
 *   - name: 线程注册时的名字
 *   - lq: 该线程的专属收件箱
 *   - th: 线程函数指针
 *   - node: 链表节点（嵌入结构体，由 list.h 管理）
 *
 * 为什么叫"类 Actor 模型"？
 *   Actor 模型的核心：每个 Actor 有一个邮箱，通过消息通信，不共享状态。
 *   Mailbox 做到了这一点——线程间不直接访问对方数据，只传消息副本。
 *   区别在于 Actor 模型是异步的、分布式的，这里只在线程级别实现。
 */

#ifndef _MAILBOX_H_
#define _MAILBOX_H_

#include <pthread.h>
#include "linkque.h"
#include "list.h"

/* MailBox System：整个系统的中央邮局 */
typedef struct mail_box_system
{
    pthread_mutex_t mutex;       // 保护所有队列操作的互斥锁
    struct list_head head;       // 线程节点链表的头（哨兵节点）
} MBS;

/* 线程函数类型：void* (*)(void*) */
typedef void* (*th_fun)(void* arg);

/*
 * LIST_DATA：线程信箱节点
 * 节点嵌入到链表中，通过 list.h 管理。
 * 查找时用 list_for_each_entry 遍历，比较 name 或 tid。
 */
typedef struct thread_node
{
    pthread_t tid;               // 线程 ID
    char name[256];              // 线程注册名
    LinkQue *lq;                 // 专属收件箱（指向堆上分配的链式队列）
    th_fun th;                   // 线程入口函数
    struct list_head node;       // 链表节点（嵌入）
} LIST_DATA;

/* ===== 接口说明 =====
 * create_mail_box_system  - 创建邮局系统
 * register_to_mail_system - 注册一个线程到邮局
 *   - 创建 LIST_DATA 节点，初始化收件箱
 *   - 把节点加入链表
 *   - 创建线程（pthread_create）
 * send_msg               - 发送消息到指定线程
 *   - 根据目标线程名查找收件箱
 *   - 自动填充发送者信息（当前线程的 tid/name）
 *   - 数据深拷贝入队
 * send_msg_as            - 以指定发送者身份发送
 *   - 和 send_msg 的区别：发送者信息用参数指定，而非 pthread_self()
 *   - 用于需要"代理发送"的场景（如 net_recv 收到数据后以 net_parse 的名义发）
 * recv_msg               - 从自己的收件箱接收一条消息
 *   - 非阻塞：没有消息立即返回 1
 * wait_all_end           - 等待所有注册线程结束
 * destroy_mail_box_system- 销毁邮局系统，释放所有资源
 */

MBS *create_mail_box_system(void);
int register_to_mail_system(MBS *mbs, char name[], th_fun th);
int send_msg(MBS *mbs, char *recvname, MAIL_DATA *data);
int send_msg_as(MBS *mbs, const char *sender_name, char *recvname, MAIL_DATA *data);
int recv_msg(MBS *mbs, MAIL_DATA *data);
int wait_all_end(MBS *mbs);
void destroy_mail_box_system(MBS *mbs);

#endif
