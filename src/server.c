/*
 * server.c - 工业设备监控服务器
 *
 * ===== 服务器设计 =====
 *
 * 服务器相对简单——它不需要 mailbox，不需要传感器采集。
 * 它只是被动接收 client 的连接和数据，打印到终端。
 *
 * 架构：
 * - TCP 监听端口 50000
 * - 每个 client 连接创建一个独立线程处理
 * - 按协议 ID 分发处理收到的数据包
 *
 * 说明：服务器只是演示用途，不包含持久化存储。
 * 实际项目中，这里的代码会被替换为数据库写入 + Web 前端展示。
 */

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "public.h"

#define PORT 50000
#define MAX_CONN 10

/* 全量接收（和 client 端完全一致） */
static int recv_safety(int fd, void *buffer, int size)
{
    int total = 0;
    char *ptr = (char *)buffer;

    while (total < size)
    {
        ssize_t ret = recv(fd, ptr + total, size - total, 0);
        if (ret < 0)
        {
            if (errno == EINTR) continue;
            return -1;
        }
        if (ret == 0) return -1;
        total += (int)ret;
    }
    return 0;
}

/* 全量发送 */
static int send_safety(int fd, const void *buffer, int size)
{
    int total = 0;
    const char *ptr = (const char *)buffer;

    while (total < size)
    {
        ssize_t ret = send(fd, ptr + total, size - total, 0);
        if (ret < 0)
        {
            if (errno == EINTR) continue;
            return -1;
        }
        if (ret == 0) return -1;
        total += (int)ret;
    }
    return 0;
}

/* 发送数据包 */
static int send_packet(int fd, int agreement_id, const void *payload, int payload_size)
{
    packet_head head;
    head.agreement_id = agreement_id;
    head.size = payload_size;

    if (send_safety(fd, &head, sizeof(head)) != 0)
        return -1;

    if (payload_size > 0 && payload != NULL &&
        send_safety(fd, payload, payload_size) != 0)
        return -1;

    return 0;
}

/*
 * handle_client - 处理一个 client 连接
 *
 * 接收 client 发来的数据包，按协议 ID 做不同处理。
 * 这是一个无限循环，直到连接断开。
 */
static void *handle_client(void *arg)
{
    int client_fd = *((int *)arg);
    free(arg);

    printf("[Server] Client connected (fd=%d)\n", client_fd);

    while (1)
    {
        packet_head head;
        void *content = NULL;

        /* 接收包头部 */
        if (recv_safety(client_fd, &head, sizeof(head)) != 0)
        {
            printf("[Server] Client disconnected (fd=%d)\n", client_fd);
            break;
        }

        /* 安全性检查 */
        if (head.size < 0 || head.size > 4096)
        {
            printf("[Server] Invalid packet size: %d (fd=%d)\n",
                   head.size, client_fd);
            break;
        }

        /* 接收载荷 */
        if (head.size > 0)
        {
            content = malloc(head.size);
            if (content == NULL)
            {
                perror("malloc fail");
                break;
            }

            if (recv_safety(client_fd, content, head.size) != 0)
            {
                printf("[Server] Client disconnected during recv (fd=%d)\n", client_fd);
                free(content);
                break;
            }
        }

        printf("[Server] Packet: id=0x%x size=%d\n", head.agreement_id, head.size);

        /* === 协议分发 === */
        switch (head.agreement_id)
        {
        case 0x00:  // 设备注册
        {
            if (head.size != (int)sizeof(device_logon_t))
            {
                printf("[Server] Invalid logon size\n");
                break;
            }
            device_logon_t *logon = (device_logon_t *)content;
            printf("[Server] ★ REGISTER: device=%u type=%s\n",
                   logon->device_id, logon->device_type);

            /* 回复注册成功 */
            device_status_t resp;
            memset(&resp, 0, sizeof(resp));
            resp.device_id = logon->device_id;
            strncpy(resp.device_type, logon->device_type,
                    sizeof(resp.device_type) - 1);
            resp.run_state = 0;

            send_packet(client_fd, 0x00, &resp, sizeof(resp));
            break;
        }

        case 0x03:  // 传感器/状态数据
        {
            if (head.size != (int)sizeof(device_status_t))
            {
                printf("[Server] Invalid status size\n");
                break;
            }
            device_status_t *st = (device_status_t *)content;
            printf("[Server] ★ STATUS: device=%u | "
                   "Temp=%.1f°C Humid=%.1f%% | "
                   "Vibr=%.2f(X=%.2f Y=%.2f Z=%.2f) m/s² | "
                   "RPM=%.0f | Current=%.1fA Volt=%.1fV\n",
                   st->device_id,
                   st->temperature, st->humidity,
                   st->vibration_total,
                   st->vibration_x, st->vibration_y, st->vibration_z,
                   st->rpm, st->current, st->voltage);

            /* 服务器侧也做报警提示 */
            if (st->vibration_total > 10.0f)
                printf("[Server] ⚠ ALARM: High vibration! %.2f m/s²\n",
                       st->vibration_total);
            if (st->temperature > 80.0f)
                printf("[Server] ⚠ ALARM: High temperature! %.1f°C\n",
                       st->temperature);
            break;
        }

        case 0x20:  // 报警信息
        {
            if (head.size != (int)sizeof(alarm_info_t))
            {
                printf("[Server] Invalid alarm size\n");
                break;
            }
            alarm_info_t *alarm = (alarm_info_t *)content;
            printf("[Server] 🚨 ALARM: device=%u id=%u "
                   "level=%d type=%d | %s\n",
                   alarm->device_id, alarm->alarm_id,
                   alarm->alarm_level, alarm->alarm_type,
                   alarm->alarm_desc);
            break;
        }

        default:
            printf("[Server] Unknown protocol: 0x%x\n", head.agreement_id);
            break;
        }

        if (content) free(content);
    }

    close(client_fd);
    printf("[Server] Client handler ended (fd=%d)\n", client_fd);
    return NULL;
}

int main(void)
{
    int server_fd;
    struct sockaddr_in server_addr;

    printf("========================================\n");
    printf("  工业设备监控服务器 v2.0\n");
    printf("========================================\n\n");

    /* 创建 socket */
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1)
    {
        perror("socket fail");
        exit(1);
    }

    /* 允许端口重用（开发调试时快速重启） */
    {
        int opt = 1;
        setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    }

    /* 绑定地址 */
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_fd, (struct sockaddr *)&server_addr,
             sizeof(server_addr)) == -1)
    {
        perror("bind fail");
        close(server_fd);
        exit(1);
    }

    printf("[Server] Listening on port %d...\n", PORT);

    if (listen(server_fd, MAX_CONN) == -1)
    {
        perror("listen fail");
        close(server_fd);
        exit(1);
    }

    /* 接受连接循环 */
    while (1)
    {
        struct sockaddr_in client_addr;
        socklen_t sin_size = sizeof(client_addr);

        int new_fd = accept(server_fd, (struct sockaddr *)&client_addr, &sin_size);
        if (new_fd == -1)
        {
            perror("accept fail");
            continue;
        }

        printf("[Server] New connection: %s:%d\n",
               inet_ntoa(client_addr.sin_addr),
               ntohs(client_addr.sin_port));

        /* 每个连接一个线程处理 */
        int *fd_ptr = malloc(sizeof(int));
        if (fd_ptr == NULL)
        {
            perror("malloc fail");
            close(new_fd);
            continue;
        }
        *fd_ptr = new_fd;

        pthread_t tid;
        if (pthread_create(&tid, NULL, handle_client, fd_ptr) != 0)
        {
            perror("pthread_create fail");
            close(new_fd);
            free(fd_ptr);
        }
        else
        {
            pthread_detach(tid);
        }
    }

    close(server_fd);
    return 0;
}
