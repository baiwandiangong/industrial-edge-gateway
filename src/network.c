/*
 * network.c - 网络通信模块实现
 *
 * ===== TCP 通信的常见陷阱 =====
 *
 * 1. TCP 是流式协议，不是消息协议
 *    send("hello") + send("world") 可能被接收方一次 recv 收到 "helloworld"
 *    也可能分多次收到 "hel" + "loworld"
 *    解决方案：自己定义消息边界 → packet_head（固定 8 字节）
 *
 * 2. send/recv 不保证一次发完/收完
 *    返回 -1 是错误
 *    返回 0  是对方关闭连接
 *    返回 < size 是只处理了部分数据（需要继续）
 *    解决方案：send_safety / recv_safety 循环
 *
 * 3. 多线程写 socket 需要保护
 *    改进后：只有 net_sender 线程写 socket，不再需要 sockfd_mutex
 */

#include "network.h"
#include <errno.h>
#include <pthread.h>
#include "main.h"
#include "mailbox.h"

/* 外部全局变量声明（在 main.c 中定义） */
extern int sockfd;
extern device_status_t device_info;
extern pthread_mutex_t device_info_mutex;

/*
 * TCP 连接初始化：端口 50000，服务器 192.168.1.3
 * （这个 IP 应该提取为配置参数，目前硬编码以保持简单）
 */
int network_init(void)
{
    printf("[network] Connecting to server 192.168.1.3:50000...\n");

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1)
    {
        perror("[network] socket fail");
        return -1;
    }

    struct sockaddr_in ser;
    memset(&ser, 0, sizeof(ser));
    ser.sin_family = AF_INET;
    ser.sin_port = htons(50000);

    if (inet_pton(AF_INET, "192.168.1.3", &ser.sin_addr) <= 0)
    {
        perror("[network] inet_pton fail");
        close(sockfd);
        sockfd = -1;
        return -1;
    }

    if (connect(sockfd, (struct sockaddr *)&ser, sizeof(ser)) == -1)
    {
        perror("[network] connect fail");
        close(sockfd);
        sockfd = -1;
        return -1;
    }

    printf("[network] Connected to server successfully\n");
    return sockfd;
}

/*
 * send_safety - 全量发送
 *
 * while(total < size) 循环：
 * 每次 send 可能只发送了一部分，需要继续发剩下的。
 * EINTR 是信号中断，不算错误，重试。
 * ret == 0 表示对方关闭连接。
 *
 * 返回 0 成功，-1 失败
 */
int send_safety(int fd, const void *buffer, int size)
{
    int total = 0;
    const char *ptr = (const char *)buffer;

    while (total < size)
    {
        ssize_t ret = send(fd, ptr + total, size - total, 0);
        if (ret < 0)
        {
            if (errno == EINTR)
                continue;     // 信号中断，重试
            return -1;
        }
        if (ret == 0)
            return -1;        // 对方关闭连接

        total += (int)ret;
    }

    return 0;
}

/*
 * recv_safety - 全量接收
 *
 * 逻辑与 send_safety 对称。
 * 注意 recv 返回 0 表示对端优雅关闭连接。
 */
int recv_safety(int fd, void *buffer, int size)
{
    int total = 0;
    char *ptr = (char *)buffer;

    while (total < size)
    {
        ssize_t ret = recv(fd, ptr + total, size - total, 0);
        if (ret < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (ret == 0)
            return -1;

        total += (int)ret;
    }

    return 0;
}

/*
 * send_packet - 发送完整数据包
 *
 * 步骤：
 * 1. 组装 packet_head（agreement_id + size）
 * 2. 发送 8 字节头部
 * 3. 发送 payload 数据
 *
 * 注意：这个函数现在只被 net_sender 线程调用，
 * 不再需要 sockfd_mutex（因为只有这一个线程写 socket）。
 */
int send_packet(int fd, int agreement_id, const void *payload, int payload_size)
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
 * get_device_status_snapshot - 线程安全读设备状态快照
 *
 * 用于主循环打印日志，不阻塞采集线程太长时间。
 */
void get_device_status_snapshot(device_status_t *status)
{
    pthread_mutex_lock(&device_info_mutex);
    memcpy(status, &device_info, sizeof(*status));
    pthread_mutex_unlock(&device_info_mutex);
}



/*
 * set_device_status - 从网络数据包更新设备状态
 *
 * 服务器可能在注册回复或状态同步请求中附带完整的设备状态。
 * 这里直接 memcpy 覆盖。
 *
 * 返回 0 成功，-1 失败（大小不匹配）
 */
int set_device_status(const void *pcontent, int size)
{
    if (size != (int)sizeof(device_status_t))
    {
        printf("[network] set_device_status: size mismatch (%d != %zu)\n",
               size, sizeof(device_status_t));
        return -1;
    }

    pthread_mutex_lock(&device_info_mutex);
    memcpy(&device_info, pcontent, sizeof(device_status_t));
    pthread_mutex_unlock(&device_info_mutex);

    printf("[network] Device status updated from server\n");
    return 0;
}

/*
 * handle_control_command - 处理服务器下发的控制命令
 *
 * 命令类型：
 * 0 → 启动（可指定目标转速）
 * 1 → 停止（转速归零）
 * 2 → 复位（清除故障码）
 */
void handle_control_command(control_cmd_t *cmd)
{
    pthread_mutex_lock(&device_info_mutex);

    printf("[control] Received command: device=%u type=%d set_rpm=%.1f\n",
           cmd->device_id, cmd->cmd_type, cmd->set_rpm);

    /* 检查命令是否发给本设备 */
    if (cmd->device_id != device_info.device_id)
    {
        pthread_mutex_unlock(&device_info_mutex);
        printf("[control] Command ignored: not for this device\n");
        return;
    }

    switch (cmd->cmd_type)
    {
    case 0:  // 启动
        device_info.run_state = 1;
        device_info.rpm = cmd->set_rpm;
        printf("[control] Device started, RPM set to %.1f\n", cmd->set_rpm);
        break;

    case 1:  // 停止
        device_info.run_state = 0;
        device_info.rpm = 0;
        printf("[control] Device stopped\n");
        break;

    case 2:  // 复位
        device_info.fault_code = 0;
        printf("[control] Device fault reset\n");
        break;

    default:
        printf("[control] Unknown command type: %d\n", cmd->cmd_type);
        break;
    }

    pthread_mutex_unlock(&device_info_mutex);
}

/*
 * send_to_net_sender - 通过 mailbox 把数据发给 net_sender 线程
 *
 * 这是改进后的核心设计：所有外发数据都经过这个函数发往 net_sender，
 * 由 net_sender 统一写 socket。
 *
 * 调用方只需提供协议 ID 和 payload 指针，
 * 函数内部封装成 MAIL_DATA 并通过 send_msg 发往 net_sender。
 *
 * 注意：MAIL_DATA.data 最大 512 字节，payload 不能超过这个大小减去头部。
 */
int send_to_net_sender(int agreement_id, const void *payload, int payload_size)
{
    packet_head head;
    MAIL_DATA mail;

    memset(&mail, 0, sizeof(mail));

    head.agreement_id = agreement_id;
    head.size = payload_size;

    /* 检查总大小是否超出 MAIL_DATA.data 容量 */
    int total_size = (int)(sizeof(packet_head) + payload_size);
    if (total_size > (int)sizeof(mail.data))
    {
        printf("[network] Packet too large for mailbox: %d bytes\n", total_size);
        return -1;
    }

    /* 组装：先放头部，再放载荷 */
    memcpy(mail.data, &head, sizeof(packet_head));
    if (payload_size > 0 && payload != NULL)
    {
        memcpy(mail.data + sizeof(packet_head), payload, payload_size);
    }
    mail.payload_len = total_size;

    /* 通过 mailbox 发给 net_sender */
    if (send_msg(g_mbs, "net_sender", &mail) != 0)
    {
        printf("[network] send_to_net_sender: failed to send to net_sender\n");
        return -1;
    }

    return 0;
}

/*
 * parse_task - 协议解析分发
 *
 * 收到一个完整数据包后，根据 agreement_id 决定怎么处理。
 * 这个函数是"网络协议处理器"，不属于严格的网络层，也不属于业务层，
 * 而是两者之间的"路由层"。
 *
 * 协议路由表：
 * 0x00 → 注册回复 → 更新设备状态（确认注册成功）
 * 0x10 → 控制命令 → handle_control_command()
 * 其他 → 打印未知协议
 */
void parse_task(packet_head *head, const void *pdata, int size)
{
    if (head == NULL || pdata == NULL || size != head->size)
    {
        printf("[network] parse_task: invalid packet\n");
        return;
    }

    switch (head->agreement_id)
    {
    case 0x00:  // 设备注册回复
        printf("[network] Received logon response\n");
        if (set_device_status(pdata, size) == 0)
            printf("[network] Logon acknowledged\n");
        else
            printf("[network] Invalid logon response\n");
        break;

    case 0x10:  // 控制命令
        if (size == (int)sizeof(control_cmd_t))
            handle_control_command((control_cmd_t *)pdata);
        else
            printf("[network] Invalid control command size: %d\n", size);
        break;

    default:
        printf("[network] Unknown protocol: 0x%x (size=%d)\n",
               head->agreement_id, size);
        break;
    }
}
