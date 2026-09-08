/*
 * list.h - Linux 内核双向循环链表移植
 *
 * ===== 设计思路 =====
 *
 * 为什么不用自己写的链表？
 * 内核链表是 C 语言中"侵入式链表"的典型范例：
 *   - 传统链表：节点里放数据 → 每种数据类型都要重写一套链表操作
 *   - 内核链表：数据里嵌节点 → 链表操作函数完全通用，只需一套
 *
 * 核心机制：container_of 宏
 *   通过结构体成员变量的地址，反推出整个结构体的基地址。
 *   这是 C 语言泛型编程的基础技巧，在 Linux 内核中无处不在。
 *
 * 使用方式：
 *   struct my_data {
 *       int value;
 *       struct list_head node;  // 链表节点嵌入到数据中
 *   };
 *
 *   // 遍历时通过 container_of 获得宿主结构体指针
 *   list_for_each_entry(pos, &head, node) { ... }
 *   // pos 直接就是 struct my_data* 类型
 *
 * 关键宏/函数说明：
 *   - LIST_HEAD(name)        : 定义并初始化链表头
 *   - list_add(new, head)    : 在 head 之后插入（头插）
 *   - list_add_tail(new,head): 在 head 之前插入（尾插）
 *   - list_del(entry)        : 从链表中移除节点
 *   - list_empty(head)       : 判断链表是否为空
 *   - list_entry(ptr,type,member): container_of 的别名
 *   - list_for_each_entry(pos, head, member): 遍历链表
 */

#ifndef _LINUX_LIST_H
#define _LINUX_LIST_H

#include <stddef.h>

#ifndef offsetof
#define offsetof(TYPE, MEMBER) ((size_t) & ((TYPE *)0)->MEMBER)
#endif

/* 毒化指针：节点被删除后指向这里，便于调试 */
#define LIST_POISON1 ((void *)0)
#define LIST_POISON2 ((void *)0)

/**
 * container_of - 通过成员指针获取宿主结构体指针
 * @ptr:    成员指针
 * @type:   宿主结构体类型
 * @member: 成员在宿主中的字段名
 *
 * 原理：成员地址 - 成员在结构体中的偏移量 = 结构体基地址
 */
#define container_of(ptr, type, member)                    \
    ({                                                     \
        const typeof(((type *)0)->member) *__mptr = (ptr); \
        (type *)((char *)__mptr - offsetof(type, member)); \
    })

/* 链表节点结构体 */
struct list_head
{
    struct list_head *next, *prev;
};

/* 静态初始化链表头 */
#define LIST_HEAD_INIT(name) \
    {                        \
        &(name), &(name)     \
    }

#define LIST_HEAD(name) struct list_head name = LIST_HEAD_INIT(name)

/* 动态初始化链表头 */
static inline void INIT_LIST_HEAD(struct list_head *list)
{
    list->next = list;
    list->prev = list;
}

/*
 * 在 prev 和 next 之间插入新节点
 * 这是 list_add 和 list_add_tail 的内部实现
 */
static inline void __list_add(struct list_head *new, struct list_head *prev,
                              struct list_head *next)
{
    next->prev = new;
    new->next = next;
    new->prev = prev;
    prev->next = new;
}

/* 头插法：插入到 head 后面（新节点成为第一个） */
static inline void list_add(struct list_head *new, struct list_head *head)
{
    __list_add(new, head, head->next);
}

/* 尾插法：插入到 head 前面（新节点成为最后一个） */
static inline void list_add_tail(struct list_head *new, struct list_head *head)
{
    __list_add(new, head->prev, head);
}

/* 断开 prev 和 next 之间的链接 */
static inline void __list_del(struct list_head *prev, struct list_head *next)
{
    next->prev = prev;
    prev->next = next;
}

/* 从链表中删除节点，并将指针毒化 */
static inline void list_del(struct list_head *entry)
{
    __list_del(entry->prev, entry->next);
    entry->next = LIST_POISON1;
    entry->prev = LIST_POISON2;
}

/* 判断链表是否为空：头节点的 next 指向自己 */
static inline int list_empty(const struct list_head *head)
{
    return head->next == head;
}

/* 从链表节点指针获取宿主结构体指针 */
#define list_entry(ptr, type, member) container_of(ptr, type, member)

/*
 * 遍历链表，pos 是 struct list_head*（低层级遍历）
 * 一般用 list_for_each_entry 更为方便
 */
#define list_for_each(pos, head) \
    for (pos = (head)->next; pos != (head); pos = pos->next)

/*
 * 遍历链表（反向）
 */
#define list_for_each_prev(pos, head) \
    for (pos = (head)->prev; pos != (head); pos = pos->prev)

/*
 * 安全遍历（带暂存节点 n，允许在遍历中删除当前节点）
 */
#define list_for_each_safe(pos, n, head)                   \
    for (pos = (head)->next, n = pos->next; pos != (head); \
         pos = n, n = pos->next)

/*
 * ★ 最常用的遍历宏：直接获得宿主结构体指针
 * 用法：list_for_each_entry(pos, &head, node) { ... }
 * pos 的类型就是宿主结构体类型，node 是链表节点字段名
 */
#define list_for_each_entry(pos, head, member)                \
    for (pos = list_entry((head)->next, typeof(*pos), member); \
         &pos->member != (head);                              \
         pos = list_entry(pos->member.next, typeof(*pos), member))

/*
 * 安全遍历宿主结构体（支持遍历中删除当前节点）
 */
#define list_for_each_entry_safe(pos, n, head, member)        \
    for (pos = list_entry((head)->next, typeof(*pos), member), \
         n = list_entry(pos->member.next, typeof(*pos), member); \
         &pos->member != (head);                              \
         pos = n, n = list_entry(pos->member.next, typeof(*pos), member))

#endif
