/*
 * storage.h - SQLite3 历史数据存储模块
 *
 * 功能：
 *   - 启动时建表（如果不存在）
 *   - 提供 storage_save() 把 device_status 写入 SQLite
 *   - 提供 storage_recent() 查询最近 N 条记录
 *   - 提供未上传数据条数 storage_pending_count()（用于断网补传）
 *
 * 用法：
 *   storage_init() 在存储线程内自动调用（建表）
 *   storage_thread() 收 mailbox 消息（sensor_collect 每秒投递 1 条）落库
 *   storage_close() 在程序退出时调用
 *
 * 为什么用 SQLite？
 *   - 嵌入式场景标准选择，单文件零配置
 *   - 比裸写 .csv 文件强：支持 SQL 查询、索引、事务
 *   - 适合断网补传：本地存盘，恢复后批量上报
 */

#ifndef STORAGE_H
#define STORAGE_H

#include "public.h"
#include <sqlite3.h>

/* 初始化：打开 db 文件、建表（不存在则创建） */
extern int  storage_init(void);

/* 插入一条记录（参数化查询，防 SQL 注入） */
extern int  storage_save(const device_status_t *status);

/* 查询最近 N 条记录（返回 JSON 字符串，调用者 free） */
extern char *storage_recent(int n);

/* 查询待补传条数 */
extern int  storage_pending_count(void);

/* 标记某条记录为已上报 */
extern int  storage_mark_uploaded(long long id);

/* 关闭数据库 */
extern void storage_close(void);

/*
 * 存储线程入口函数（消息驱动）
 * - 被 main.c 注册启动
 * - recv_msg 接收 sensor_collect 投递的 device_status_t 快照并写入 SQLite
 */
extern void *storage_thread(void *arg);

#endif
