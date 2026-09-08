/*
 * main.c - 主程序（架构总控）
 *
 * ===== 这是整个项目的核心 =====
 *
 * main() 的作用不是写业务逻辑，而是"搭架子"——把各个模块组装起来，
 * 让数据按照正确的路径流动。
 *
 * ===== 数据流全景 =====
 *
 *  ┌─────────────────────────────────────────────────────┐
 *  │                     main()                          │
 *  │  1. 初始化 device_info                              │
 *  │  2. create_mail_box_system()                        │
 *  │  3. network_init()                                  │
 *  │  4. register 线程：                                  │
 *  │     ├─ "net_receiver"   (TCP 接收)                  │
 *  │     ├─ "net_sender"     (TCP 发送 ← 唯一写 socket)  │
 *  │     ├─ "net_parser"     (协议解析)                   │
 *  │     ├─ "sensor_collect" (传感器采集)                 │
 *  │     ├─ "storage" (SQLite 历史落库)                   │
 *  │     └─ "device_monitor" (状态监测 + 报警)            │
 *  │  5. 主循环：打印状态                                 │
 *  └─────────────────────────────────────────────────────┘
 *
 * ===== 线程间数据流（值得反复看） =====
 *
 * 【上行：传感器数据 → 服务器】
 *   sensor_collect ──send_to_net_sender()──→ [mailbox] ──→ net_sender ──send_packet()──→ TCP → 服务器
 *   device_monitor ──send_to_net_sender()──→ [mailbox] ──→ net_sender ──send_packet()──→ TCP → 服务器
 *
 * 【下行：服务器控制命令 → 设备】
 *   服务器 ──TCP──→ net_recv ──send_msg("net_parser")──→ [mailbox] ──→ net_parse ──handle_control_command()──→ device_info
 *
 * 【报警信息】
 *   device_monitor ──send_to_net_sender()──→ [mailbox] ──→ net_sender ──send_packet()──→ TCP → 服务器
 *
 * 【本地历史存储：采集数据本地落库（不经网络）】
 *   sensor_collect ──send_msg("storage")──→ [mailbox] ──→ storage_thread ──sqlite3──→ /tmp/sensor_history.db
 *   （100Hz 采集降频为每秒 1 行；storage 由消息驱动、不再定时采样 device_info）
 *
 * ===== 改进总结（和原项目对比） =====
 *
 * 原项目问题                              → 改进
 * ├─ sensor 和 device_monitor 直写 socket  → 统一走 mailbox → net_sender
 * ├─ 需要 sockfd_mutex 保护多线程写 socket → 单线程写，无需锁
 * ├─ trash.c 是遗留垃圾代码               → 已清理
 * └─ 无重连机制                           → 后续可加（在 recv_safety 失败后触发重连）
 */

/* =================================================================
 * 头文件包含
 * =================================================================
 * 注意：mailbox.h 必须在 main.h 之前，因为 main.h 中 extern 了
 * MBS* 指针，而 MBS 类型在 mailbox.h 中定义。
 * 但如果 main.h 只声明 extern MBS *g_mbs，其实只需要前向声明。
 * 这里为了清晰，main.h 中 #include "mailbox.h"。
 * 同时注意包含顺序，避免循环依赖。
 * =================================================================
 */
#include "mailbox.h"     // MBS, send_msg, recv_msg, register_to_mail_system
#include "main.h"        // 全局变量声明
#include "network.h"     // 网络初始化、协议解析、send_to_net_sender
#include "industrial.h"  // device_monitor_thread
#include "sensor.h"      // sensor_collect_thread
#include "storage.h"     // storage_thread（SQLite 存储）

#include <pthread.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

/* =================================================================
 * 全局变量定义（这是变量的"出生地"，其他文件通过 main.h 引用）
 * =================================================================
 */

int sockfd = -1;                              // TCP socket 描述符
device_status_t device_info;                   // 设备状态（全局共享）
pthread_mutex_t device_info_mutex = PTHREAD_MUTEX_INITIALIZER;  // 保护 device_info
MBS *g_mbs = NULL;                             // Mailbox 系统（全局唯一）

/* =================================================================
 * 辅助函数：将收到的网络包封装到 MAIL_DATA 并投递到 net_parser
 * =================================================================
 *
 * 为什么收数据的是 net_recv，拆包的是 net_parser？
 * 把"收"和"解析"分开，net_recv 只负责从 TCP 取数据，
 * 然后丢给 net_parser 去解析。这样：
 * - net_recv 可以尽快返回继续收下一包（不阻塞 IO）
 * - 如果解析逻辑变复杂（比如增加数据验证），不会影响接收速度
 */
static void enqueue_packet_for_parser(packet_head *head, const void *payload)
{
    MAIL_DATA mail;
    memset(&mail, 0, sizeof(mail));

    int total_size = (int)(sizeof(packet_head) + head->size);
    if (total_size > (int)sizeof(mail.data))
    {
        printf("[net_recv] Packet too large for mailbox: %d bytes\n", head->size);
        return;
    }

    /* 组装 MAIL_DATA：先放头部，再放载荷 */
    memcpy(mail.data, head, sizeof(packet_head));
    if (head->size > 0 && payload != NULL)
    {
        memcpy(mail.data + sizeof(packet_head), payload, head->size);
    }
    mail.payload_len = total_size;

    /* 发往 net_parser 线程 */
    if (send_msg(g_mbs, "net_parser", &mail) != 0)
    {
        printf("[net_recv] Failed to send packet to net_parser\n");
    }
}

/* =================================================================
 * 线程：net_recv_thread —— 网络数据接收者
 * =================================================================
 *
 * 职责：
 *   从 TCP socket 循环接收数据包（头部 + 载荷），
 *   收到完整包后封装成 MAIL_DATA 发给 net_parser 线程。
 *
 * 用 mailbox 而不是直接调用 parse_task 的原因：
 *   如果 net_recv 直接调用 parse_task，那么 parse 阶段的耗时
 *   会阻塞 recv，造成 IO 延迟。通过 mailbox 解耦后，
 *   接收和解析是两个独立的线程，互不阻塞。
 *
 * 为什么每个包都要检查 size 合法性？
 *   如果 server 发送了损坏数据或恶意攻击，size 可能超大或为负数。
 *   不做检查的话 memcpy/malloc 会出问题。这是一个基本的安全实践。
 */
void *net_recv_thread(void *arg)
{
    (void)arg;
    printf("[net_recv] Thread started\n");

    while (1)
    {
        packet_head head;
        char payload[512];  // 和 MAIL_DATA.data 保持一致的容量

        /* 接收 8 字节头部 */
        if (recv_safety(sockfd, &head, sizeof(head)) != 0)
        {
            printf("[net_recv] Connection lost or recv error\n");
            break;
        }

        /* 校验：防止恶意/损坏的数据 */
        if (head.size < 0 || head.size > (int)sizeof(payload))
        {
            printf("[net_recv] Invalid payload size: %d\n", head.size);
            break;
        }

        /* 接收载荷（如果有） */
        if (head.size > 0)
        {
            if (recv_safety(sockfd, payload, head.size) != 0)
            {
                printf("[net_recv] Payload recv error\n");
                break;
            }
        }

        printf("[net_recv] Received packet: id=0x%x size=%d\n",
               head.agreement_id, head.size);

        /* 通过 mailbox 发给 net_parser 处理 */
        enqueue_packet_for_parser(&head,
                                  head.size > 0 ? payload : NULL);
    }

    printf("[net_recv] Thread exiting\n");
    return NULL;
}

/* =================================================================
 * 线程：net_sender_thread —— 网络数据发送者（唯一写 socket 的线程）
 * =================================================================
 *
 * 这是架构改进的核心：
 * ★ 整个项目只有这一个线程写 socket ★
 *
 * 其他所有需要发送数据的线程（sensor_collect、device_monitor、alarm），
 * 都把数据通过 send_msg("net_sender", &mail) 发到这个线程，
 * 由 net_sender 统一调用 send_packet() 写 socket。
 *
 * 好处：
 * 1. 不需要 sockfd_mutex（只有一个线程操作）
 * 2. 可以在这里统一加流量控制、发送队列管理
 * 3. 发送失败的处理逻辑集中在一处
 *
 * 工作流：
 *   recv_msg(mbs, &mail) ← 从 mailbox 收消息
 *   → 拆出 packet_head + payload
 *   → send_packet(sockfd, agreement_id, payload, payload_size)
 *
 * 非阻塞接收：没有消息时 sleep 10ms 后重试
 */
void *net_sender_thread(void *arg)
{
    (void)arg;
    printf("[net_sender] Thread started\n");

    while (1)
    {
        MAIL_DATA mail;
        memset(&mail, 0, sizeof(mail));

        /* 从 mailbox 接收消息（非阻塞） */
        if (recv_msg(g_mbs, &mail) != 0)
        {
            usleep(10000);   // 10ms 重试
            continue;
        }

        /* 验证消息合法性 */
        if (mail.payload_len < (int)sizeof(packet_head))
        {
            printf("[net_sender] Invalid payload length: %d\n", mail.payload_len);
            continue;
        }

        packet_head *head = (packet_head *)mail.data;
        char *payload = mail.data + sizeof(packet_head);
        int payload_size = mail.payload_len - (int)sizeof(packet_head);

        /* 校验载荷大小和头部声明的一致 */
        if (payload_size != head->size)
        {
            printf("[net_sender] Size mismatch: head=%d actual=%d\n",
                   head->size, payload_size);
            continue;
        }

        /* ★ 唯一写 socket 的地方 ★ */
        if (send_packet(sockfd, head->agreement_id, payload, payload_size) != 0)
        {
            perror("[net_sender] send_packet failed");
            break;  // 连接断开，退出
        }

        printf("[net_sender] Sent packet: id=0x%x size=%d\n",
               head->agreement_id, head->size);
    }

    printf("[net_sender] Thread exiting\n");
    return NULL;
}

/* =================================================================
 * 线程：net_parse_thread —— 协议解析器
 * =================================================================
 *
 * 从 mailbox 接收消息，调用 parse_task() 做协议分发。
 *
 * parse_task 根据 agreement_id 决定：
 *   - 0x00 → 注册回复 → 更新 device_info
 *   - 0x10 → 控制命令 → handle_control_command
 *   - 其他 → 打印未知
 *
 * 这个线程和 net_recv 通过 mailbox 解耦，
 * 所以即使解析逻辑变复杂，也不会拖慢网络接收。
 */
void *net_parse_thread(void *arg)
{
    (void)arg;
    printf("[net_parse] Thread started\n");

    while (1)
    {
        MAIL_DATA mail;
        memset(&mail, 0, sizeof(mail));

        if (recv_msg(g_mbs, &mail) != 0)
        {
            usleep(10000);
            continue;
        }

        if (mail.payload_len < (int)sizeof(packet_head))
        {
            printf("[net_parse] Invalid payload length: %d\n", mail.payload_len);
            continue;
        }

        packet_head *head = (packet_head *)mail.data;
        void *payload = mail.data + sizeof(packet_head);
        int payload_size = mail.payload_len - (int)sizeof(packet_head);

        /* 调用 network.c 中的协议解析函数 */
        parse_task(head, payload, payload_size);
    }

    printf("[net_parse] Thread exiting\n");
    return NULL;
}

/* =================================================================
 * 启动网络服务
 * =================================================================
 *
 * 三步曲：
 * 1. network_init() - 建立 TCP 连接
 * 2. register 三个网络线程到 mailbox
 * 3. 发送设备注册请求
 */
static int star_network_service(void)
{
    printf("[main] Starting network service...\n");

    sockfd = network_init();
    if (sockfd < 0)
    {
        printf("[main] network_init failed\n");
        return -1;
    }

    /* 注册网络线程 */
    register_to_mail_system(g_mbs, "net_receiver", net_recv_thread);
    register_to_mail_system(g_mbs, "net_sender",   net_sender_thread);
    register_to_mail_system(g_mbs, "net_parser",   net_parse_thread);

    /*
     * 发送设备注册请求
     *
     * 注意：注册请求通过 send_msg_as 发给 net_sender，
     * 发送者身份标注为 "net_sender" 自己。
     * 这样 net_sender 收到后会直接发出去。
     */
    {
        device_logon_t logon_data;
        packet_head head;
        MAIL_DATA mail;

        memset(&logon_data, 0, sizeof(logon_data));
        pthread_mutex_lock(&device_info_mutex);
        logon_data.device_id = device_info.device_id;
        logon_data.timestamp = time(NULL);
        strncpy(logon_data.device_type, device_info.device_type,
                sizeof(logon_data.device_type) - 1);
        pthread_mutex_unlock(&device_info_mutex);

        head.agreement_id = 0x00;
        head.size = (int)sizeof(logon_data);

        memset(&mail, 0, sizeof(mail));
        memcpy(mail.data, &head, sizeof(head));
        memcpy(mail.data + sizeof(head), &logon_data, sizeof(logon_data));
        mail.payload_len = (int)(sizeof(head) + sizeof(logon_data));

        send_msg_as(g_mbs, "net_sender", "net_sender", &mail);
    }

    return 0;
}

/* =================================================================
 * main() - 程序入口
 * =================================================================
 *
 * 执行流程：
 *   1. 打印启动信息
 *   2. 初始化 device_info（默认值）
 *   3. 创建 mailbox 系统
 *   4. 启动网络服务（建立连接 + 注册网络线程）
 *   5. 注册业务线程
 *   6. 主循环：打印状态日志
 *   7. 等待线程结束（理论上不会执行到这里，因为线程都是无限循环）
 *
 * 为什么 main 函数很简单？
 *   复杂的逻辑都被封装到各个模块中，main 只做"搭积木"的工作。
 *   这是一种"组合而非继承"的设计思想——每个模块独立可测，
 *   main 负责把它们组合成完整系统。
 */
int main(int argc, const char *argv[])
{
    (void)argc;
    (void)argv;

    /* ===== 1. 启动横幅 ===== */
    printf("========================================\n");
    printf("  工业设备预维护边缘网关 v2.0\n");
    printf("  处理器: i.MX6ULL\n");
    printf("  架构:   Mailbox Actor Model\n");
    printf("========================================\n\n");

    srand((unsigned int)time(NULL));

    /* ===== 2. 初始化设备信息 ===== */
    memset(&device_info, 0, sizeof(device_info));
    pthread_mutex_lock(&device_info_mutex);
    device_info.device_id = 1;
    strncpy(device_info.device_type, "Motor",
            sizeof(device_info.device_type) - 1);
    device_info.run_state = 0;
    pthread_mutex_unlock(&device_info_mutex);

    /* ===== 3. 创建 Mailbox 系统 ===== */
    printf("[main] Creating Mailbox System...\n");
    g_mbs = create_mail_box_system();
    if (g_mbs == NULL)
    {
        printf("[main] Failed to create mailbox system\n");
        return -1;
    }
    printf("[main] Mailbox System created\n");

    /* ===== 4. 启动网络服务 ===== */
    if (star_network_service() != 0)
    {
        printf("[main] Failed to start network service\n");
        destroy_mail_box_system(g_mbs);
        return -1;
    }

    /* ===== 5. 注册业务线程 ===== */
    printf("[main] Starting business threads...\n");
    register_to_mail_system(g_mbs, "device_monitor",  device_monitor_thread);
    /* 注意注册顺序：storage 先于 sensor_collect。
     * mailbox 按"线程名"寻址，sensor_collect 一启动就向 storage 投递落库消息，
     * 接收方必须已注册（类 Actor：先注册、后投递），否则 send_msg 找不到目标。 */
    register_to_mail_system(g_mbs, "storage",         storage_thread);  /* SQLite 历史数据存储 */
    register_to_mail_system(g_mbs, "sensor_collect",  sensor_collect_thread);
    printf("[main] All threads registered\n\n");

    /* ===== 6. 主循环：打印状态日志 ===== */
    printf("[main] Entering main loop\n");
    while (1)
    {
        device_status_t snapshot;
        get_device_status_snapshot(&snapshot);

        printf("[main] State=%s | Temp=%.1f°C | Vibr=%.2f m/s² | RPM=%.0f\n",
               snapshot.run_state == 1 ? "Running" :
               snapshot.run_state == 2 ? "Fault"   : "Stopped",
               snapshot.temperature,
               snapshot.vibration_total,
               snapshot.rpm);

        sleep(5);
    }

    /* ===== 7. 清理（不会执行到这里） ===== */
    wait_all_end(g_mbs);
    destroy_mail_box_system(g_mbs);
    return 0;
}
