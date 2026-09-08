/*
 * mailbox.c - 线程间消息传递系统实现
 *
 * ===== 关键实现细节 =====
 *
 * 1. 非阻塞接收
 *    recv_msg 不阻塞，没有消息就返回 1。
 *    调用方需要自己做轮询（usleep + continue）。
 *    为什么这么设计？
 *    - 保持简单，不需要条件变量
 *    - 线程可以在轮询间隙做其他轻量级工作
 *    - 缺点是浪费 CPU（usleep 10ms 可接受）
 *
 * 2. 数据深拷贝
 *    EnterLinkQue 内部用 memcpy 复制数据。
 *    发送方传完消息可以立即释放/重用原数据。
 *
 * 3. 互斥锁粒度
 *    锁只保护队列操作（EnterLinkQue/GetHeadLinkQue/QuitLinkQue），
 *    不保护数据本身。这样可以减少锁竞争。
 *
 * 4. 线程注册失败处理
 *    register_to_mail_system 失败时（malloc 失败），
 *    不会有线程被创建，不会产生孤儿线程。
 */

#include "mailbox.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

MBS *create_mail_box_system(void)
{
    MBS *m_mbs = malloc(sizeof(MBS));
    if (NULL == m_mbs)
    {
        perror("create_mail_box_system malloc");
        return NULL;
    }

    /* 初始化链表头（哨兵节点自指） */
    INIT_LIST_HEAD(&m_mbs->head);

    /* 初始化保护锁 */
    pthread_mutex_init(&m_mbs->mutex, NULL);

    return m_mbs;
}

/*
 * register_to_mail_system - 注册一个线程
 *
 * 做三件事：
 * 1. malloc 分配 LIST_DATA 节点 + 内部的 LinkQue
 * 2. 把节点加入 MBS 的线程链表
 * 3. pthread_create 启动线程
 *
 * 注意：pthread_create 之后立即返回，不等线程跑起来。
 *       线程真正的入口参数是 th 函数指针。
 */
int register_to_mail_system(MBS *mbs, char name[], th_fun th)
{
    LIST_DATA *list_node = malloc(sizeof(LIST_DATA));
    if (NULL == list_node)
    {
        perror("register_to_mail_system malloc");
        return 1;
    }

    strncpy(list_node->name, name, sizeof(list_node->name) - 1);
    list_node->name[sizeof(list_node->name) - 1] = '\0';
    list_node->lq = CreateLinkQue();  // 创建专属收件箱
    list_node->th = th;

    /* 把节点加入 MBS 链表（尾插，保持注册顺序） */
    list_add_tail(&list_node->node, &mbs->head);

    /*
     * 创建线程，传入 NULL 参数。
     * 线程的执行体就是 th（在注册时指定的线程函数）。
     * 解释：为什么 register 要自己负责创建线程？
     *   因为 register 是最清楚"这个线程需要收件箱"的地方。
     *   如果外面创建线程再注册，收件箱可能在 start 之前还没准备好。
     */
    if (pthread_create(&list_node->tid, NULL, th, NULL) != 0)
    {
        perror("register_to_mail_system pthread_create");
        list_del(&list_node->node);     // 从链表删除
        DestroyLinkQue(list_node->lq);  // 释放收件箱
        free(list_node);                 // 释放节点
        return 1;
    }

    printf("[mailbox] Thread '%s' registered (tid=%lu)\n",
           name, (unsigned long)list_node->tid);
    return 0;
}

int wait_all_end(MBS *mbs)
{
    LIST_DATA *pos, *q;

    /*
     * 遍历线程链表，pthread_join 等待每个线程结束。
     * 使用 list_for_each_entry_safe 支持遍历中删除。
     */
    list_for_each_entry_safe(pos, q, &mbs->head, node)
    {
        pthread_join(pos->tid, NULL);
    }
    return 0;
}

/*
 * find_node_byname - 根据线程名查找线程节点
 * 遍历 MBS 链表，逐个比较 name 字段。
 * O(n) 复杂度，但 n 通常只有 5-10 个线程，不是瓶颈。
 */
static LIST_DATA *find_node_byname(MBS *mbs, char *name)
{
    LIST_DATA *pos, *q;

    list_for_each_entry_safe(pos, q, &mbs->head, node)
    {
        if (0 == strcmp(pos->name, name))
        {
            return pos;
        }
    }
    return NULL;
}

/*
 * find_node_byid - 根据线程 ID 查找线程节点
 * 这个函数是从原项目中保留的，但由于 pthread_t 不保证唯一，
 * 实际使用中 find_node_byname 更可靠。
 */
static LIST_DATA *find_node_byid(MBS *mbs, pthread_t id)
{
    LIST_DATA *pos, *q;

    list_for_each_entry_safe(pos, q, &mbs->head, node)
    {
        if (pos->tid == id)
        {
            return pos;
        }
    }
    return NULL;
}

/*
 * send_msg - 发送消息到指定线程
 *
 * 执行流程：
 * 1. 通过 pthread_self() 获取发送者 tid
 * 2. 查找发送者在链表中的节点，获取其 name
 * 3. 查找目标线程的节点，获取其 tid 和收件箱
 * 4. 填写 MAIL_DATA 头信息
 * 5. 深拷贝入队
 *
 * 这里的数据拷贝有两层：
 * - MAIL_DATA 的 data[] 是静态数组，memcpy 直接复制所有内容
 * - EnterLinkQue 内部还会再 memcpy 一次到队列节点
 * 第一次拷贝确保发送方可以继续修改 mail 参数，第二次是队列的规范操作。
 */
int send_msg(MBS *mbs, char *recvname, MAIL_DATA *data)
{
    /* 找发送者（当前线程） */
    LIST_DATA *myself = find_node_byid(mbs, pthread_self());
    if (NULL == myself)
    {
        fprintf(stderr, "[mailbox] send_msg: cannot find sender (tid=%lu)\n",
                (unsigned long)pthread_self());
        return 1;
    }

    /* 填充发送者信息 */
    data->id_of_sender = pthread_self();
    strncpy(data->name_of_sender, myself->name, sizeof(data->name_of_sender) - 1);

    /* 找目标线程 */
    LIST_DATA *recver = find_node_byname(mbs, recvname);
    if (NULL == recver)
    {
        fprintf(stderr, "[mailbox] send_msg: cannot find receiver '%s'\n", recvname);
        return 1;
    }

    /* 填充接收者信息 */
    data->id_of_recver = recver->tid;
    strncpy(data->name_of_recver, recver->name, sizeof(data->name_of_recver) - 1);

    /* 临界区：入队（深拷贝） */
    pthread_mutex_lock(&mbs->mutex);
    EnterLinkQue(recver->lq, data);
    pthread_mutex_unlock(&mbs->mutex);

    return 0;
}

/*
 * send_msg_as - 以指定发送者身份发送消息
 *
 * 和 send_msg 的区别：
 * send_msg 是"当前线程"发给目标线程
 * send_msg_as 是"sender_name"发给目标线程（模拟身份）
 *
 * 为什么要这个函数？
 * 看 net_recv 线程的工作流：
 *   TCP 收到数据 → 封装成 MAIL_DATA → 发给 net_parse
 * 这里的数据来源是网络，不是 net_recv 线程自己产生的。
 * 用 send_msg_as 可以标注"真正的数据来源"。
 *
 * 在改进后的架构中，所有外发数据都由 net_sender 统一处理，
 * 其他线程通过 send_msg 发给 "net_sender"，
 * 这个函数主要用于 net_recv 收到数据后转发给 net_parse。
 */
int send_msg_as(MBS *mbs, const char *sender_name, char *recvname, MAIL_DATA *data)
{
    LIST_DATA *sender = find_node_byname(mbs, (char *)sender_name);
    if (NULL == sender)
    {
        fprintf(stderr, "[mailbox] send_msg_as: cannot find sender '%s'\n", sender_name);
        return 1;
    }

    LIST_DATA *recver = find_node_byname(mbs, recvname);
    if (NULL == recver)
    {
        fprintf(stderr, "[mailbox] send_msg_as: cannot find receiver '%s'\n", recvname);
        return 1;
    }

    data->id_of_sender = sender->tid;
    strncpy(data->name_of_sender, sender->name, sizeof(data->name_of_sender) - 1);
    data->id_of_recver = recver->tid;
    strncpy(data->name_of_recver, recver->name, sizeof(data->name_of_recver) - 1);

    pthread_mutex_lock(&mbs->mutex);
    EnterLinkQue(recver->lq, data);
    pthread_mutex_unlock(&mbs->mutex);

    return 0;
}

/*
 * recv_msg - 从自己的收件箱接收一条消息
 *
 * 非阻塞设计：
 * - 有消息：数据被拷贝到 data 指向的缓冲区，返回 0
 * - 无消息：立即返回 1（调用方做 usleep 延迟重试）
 *
 * peek + 出队 两步：
 * 先 GetHeadLinkQue 查看队首，有数据则 memcpy 出来，再 QuitLinkQue。
 * 为什么不直接用 pop（返回节点数据并释放）？
 * 因为队列操作在锁里面，memcpy 也在锁里面，尽量减少锁的持有时间。
 * 如果改成 pop 返回 malloc 的数据，调用方还得 free，容易遗漏。
 */
int recv_msg(MBS *mbs, MAIL_DATA *data)
{
    /* 找当前线程对应的节点 */
    LIST_DATA *myself = find_node_byid(mbs, pthread_self());
    if (NULL == myself)
    {
        fprintf(stderr, "[mailbox] recv_msg: cannot find myself (tid=%lu)\n",
                (unsigned long)pthread_self());
        return 1;
    }

    pthread_mutex_lock(&mbs->mutex);

    MAIL_DATA *tmp = GetHeadLinkQue(myself->lq);
    if (NULL == tmp)
    {
        /* 收件箱为空 */
        pthread_mutex_unlock(&mbs->mutex);
        return 1;  // 返回 1 表示"没有消息"
    }

    /* 把消息复制到调用方提供的缓冲区 */
    memcpy(data, tmp, sizeof(MAIL_DATA));
    QuitLinkQue(myself->lq);  // 出队释放

    pthread_mutex_unlock(&mbs->mutex);

    return 0;
}

void destroy_mail_box_system(MBS *mbs)
{
    LIST_DATA *pos, *q;

    /*
     * 遍历链表，逐个释放：
     * 1. list_del 从链表移除
     * 2. DestroyLinkQue 释放收件箱
     * 3. free 释放节点
     */
    list_for_each_entry_safe(pos, q, &mbs->head, node)
    {
        list_del(&pos->node);
        if (pos->lq != NULL)
        {
            DestroyLinkQue(pos->lq);
        }
        free(pos);
    }

    pthread_mutex_destroy(&mbs->mutex);
    free(mbs);
}
