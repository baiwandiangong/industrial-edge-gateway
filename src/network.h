/*
 * network.h - 网络通信模块
 *
 * ===== 模块职责 =====
 *
 * network 模块处理所有 TCP 网络相关的操作：
 * 1. 建立 TCP 连接到服务器
 * 2. 全量发送/接收（解决 TCP 粘包/半包问题）
 * 3. 协议分包/组包
 *
 * 不负责的事情：
 * - 协议解析（那是 parse_task 的职责，在 main.c 中）
 * - 数据采集（那是 sensor 的职责）
 * - 业务逻辑（那是 industrial 的职责）
 *
 * ===== 线程安全设计 =====
 *
 * 所有 socket 写入操作都通过 mailox → net_sender 线程统一完成。
 * net_sender 是唯一写 socket 的线程，所以不需要 sockfd_mutex。
 * 读取操作由 net_recv 线程独享。
 *
 * 这样设计的好处：
 * 1. 不存在多线程竞争 socket 的问题
 * 2. 需要给数据加头、加密、压缩时，只需改 net_sender 一处
 * 3. 出错处理集中（net_sender 写失败 → 统一处理重连/退出）
 */

#ifndef NETWORK_H
#define NETWORK_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <netinet/in.h>
#include <unistd.h>
#include "public.h"

/*
 * 网络初始化：建立到服务器的 TCP 连接
 * 返回 socket 描述符，失败返回 -1
 */
extern int network_init(void);

/*
 * 安全收发（处理 TCP 半包/粘包）
 *
 * TCP 是流式协议，send/recv 不保证一次性发送/接收完所有数据。
 * send_safety 循环发送直到全部发完。
 * recv_safety 循环接收直到收够指定字节数。
 *
 * 返回 0 成功，-1 失败
 */
extern int send_safety(int fd, const void *buffer, int size);
extern int recv_safety(int fd, void *buffer, int size);

/*
 * 发送一个完整的数据包（自动加上 packet_head）
 *
 * 调用方只需提供：
 * - agreement_id: 协议 ID
 * - payload: 载荷数据指针
 * - payload_size: 载荷大小
 *
 * 函数内部组装 packet_head + payload 一次发送。
 */
extern int send_packet(int fd, int agreement_id, const void *payload, int payload_size);

/*
 * 设备注册鉴权请求
 * 连接建立后立即发送，告诉服务器"我是谁"
 */
extern int device_logon_request(void);

/*
 * 设备状态快照（线程安全地读取 device_info）
 * 用于主循环打印日志，不干扰采集线程。
 */
extern void get_device_status_snapshot(device_status_t *status);

/*
 * 设置设备状态（从网络数据包更新设备状态）
 * 用于处理服务器下发的状态更新。
 */
extern int set_device_status(const void *pcontent, int size);

/*
 * 处理服务器下发的控制命令
 */
extern void handle_control_command(control_cmd_t *cmd);

/*
 * 通过 mailbox 把数据发给 net_sender 线程（统一发送入口）
 * 所有需要往外发数据的线程都调用这个函数，而不是直接写 socket
 */
extern int send_to_net_sender(int agreement_id, const void *payload, int payload_size);

/*
 * 网络协议解析：根据 agreement_id 分发处理
 *
 * 这是网络层和业务层的分界线：
 * - 网络层负责"怎么收/发"
 * - parse_task 负责"收到后做什么"
 *
 * 这个函数被 net_parse 线程调用（通过 mailbox 接收消息）。
 */
extern void parse_task(packet_head *head, const void *pdata, int size);

#endif
