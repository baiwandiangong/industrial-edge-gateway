/*
 * main.h - 主程序公共声明
 *
 * 这里是全局变量的"注册中心"。
 * 所有需要在多个 .c 文件间共享的变量，都在这里用 extern 声明，
 * 在 main.c 中定义。其他文件 #include "main.h" 即可访问。
 *
 * 为什么不用一个全局结构体？
 * 分开声明更直观，且某些变量（如 mutex 必须在 main.c 初始化）需要控制初始化时机。
 */

#ifndef MAIN_H
#define MAIN_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <math.h>
#include "public.h"
#include "mailbox.h"

/* 设备状态（全局共享，多线程读写，由 device_info_mutex 保护） */
extern device_status_t device_info;
extern pthread_mutex_t device_info_mutex;

/* socket 描述符（只有 net_recv 和 net_sender 线程使用） */
extern int sockfd;

/* mailbox 系统（所有消息传递的总线） */
extern MBS *g_mbs;

#endif
