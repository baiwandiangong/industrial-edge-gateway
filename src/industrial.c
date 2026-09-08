/*
 * industrial.c - 工业设备业务逻辑实现
 *
 * ===== 改进：统一走 mailbox =====
 *
 * 原项目中，device_monitor_thread 和 generate_alarm 直接调用 send_packet(sockfd)
 * 写 socket。改进后改为调用 send_to_net_sender()，通过 mailbox 发往 net_sender。
 *
 * 报警阈值（根据实际设备可以调整）：
 *   振动总超 10.0 m/s² → 报警（严重电机振动）
 *   温度超 80°C       → 报警（电机过热）
 *   电流超 100.0A     → 报警（过载）
 */

#include "industrial.h"
#include "mailbox.h"
#include "main.h"
#include "network.h"

extern MBS *g_mbs;
extern device_status_t device_info;
extern pthread_mutex_t device_info_mutex;
extern int sockfd;

/*
 * check_alarm_condition - 检查设备状态是否触发报警
 *
 * 返回：0=正常，1=振动异常，2=温度异常，3=电流异常
 */
int check_alarm_condition(device_status_t *status)
{
    if (status->vibration_total > 10.0f)
        return 1;  // 振动异常

    if (status->temperature > 80.0f)
        return 2;  // 温度异常

    if (status->current > 100.0f)
        return 3;  // 电流异常

    return 0;  // 一切正常
}

/*
 * generate_alarm - 生成报警并通过 mailbox 上报
 *
 * alarm_info_t 的数据通过 send_to_net_sender 发往 net_sender，
 * 协议 ID 为 0x20（报警信息）。
 */
void generate_alarm(unsigned int device_id,
                    unsigned char level,
                    unsigned char type,
                    const char *desc)
{
    alarm_info_t alarm;
    memset(&alarm, 0, sizeof(alarm));

    alarm.device_id = device_id;
    alarm.alarm_id = (unsigned int)rand();
    alarm.alarm_level = level;
    alarm.alarm_type = type;
    alarm.alarm_time = time(NULL);
    strncpy(alarm.alarm_desc, desc, sizeof(alarm.alarm_desc) - 1);

    printf("[ALARM] Device %d | Level %d | Type %d: %s\n",
           device_id, level, type, desc);

    /* 通过 mailbox 发给 net_sender，统一走网络 */
    if (send_to_net_sender(0x20, &alarm, sizeof(alarm)) != 0)
    {
        printf("[industrial] Failed to send alarm via mailbox\n");
    }
}

/*
 * device_monitor_thread - 设备状态监测线程
 *
 * ===== 工作流 =====
 *
 * 每 3 秒检查一次 device_info，做两件事：
 *
 * 1. 状态变化上报
 *    如果 run_state / temperature / humidity / vibration_total 有变化，
 *    则上报 0x03（设备状态数据）。
 *    这样做比无差别上报省带宽。
 *
 * 2. 报警检测
 *    如果状态数据超过报警阈值，生成 0x20（报警信息）上报。
 *
 * 为什么主循环不用 mailbox 接收消息？
 * 因为这个线程是被动轮询型——它不依赖其他线程的消息，
 * 而是主动定期检查全局状态。
 */
void *device_monitor_thread(void *arg)
{
    (void)arg;
    printf("[device_monitor] Thread started\n");

    /* 记录上次监测值，用于检测变化 */
    short last_state = -1;
    float last_temp = 0;
    float last_humid = 0;
    float last_vibr = 0;

    while (1)
    {
        device_status_t snapshot;

        /* 读快照（mutex 保护） */
        pthread_mutex_lock(&device_info_mutex);
        memcpy(&snapshot, &device_info, sizeof(snapshot));
        pthread_mutex_unlock(&device_info_mutex);

        /* ===== 1. 状态变化检测 ===== */
        int changed = (last_state != snapshot.run_state ||
                       last_temp  != snapshot.temperature ||
                       last_humid != snapshot.humidity ||
                       last_vibr  != snapshot.vibration_total);

        if (changed)
        {
            last_state = snapshot.run_state;
            last_temp  = snapshot.temperature;
            last_humid = snapshot.humidity;
            last_vibr  = snapshot.vibration_total;

            printf("[device_monitor] Status changed: state=%d temp=%.1f humidity=%.1f vibr=%.2f\n",
                   snapshot.run_state, snapshot.temperature,
                   snapshot.humidity, snapshot.vibration_total);

            /* 通过 mailbox 上报 */
            if (send_to_net_sender(0x03, &snapshot, sizeof(snapshot)) != 0)
            {
                printf("[device_monitor] Failed to send status\n");
            }
        }

        /* ===== 2. 报警检测 ===== */
        int alarm_type = check_alarm_condition(&snapshot);
        if (alarm_type != 0)
        {
            char desc[64];
            switch (alarm_type)
            {
            case 1:
                snprintf(desc, sizeof(desc),
                         "振动异常：%.2f m/s²", snapshot.vibration_total);
                generate_alarm(snapshot.device_id, 2, 1, desc);
                break;
            case 2:
                snprintf(desc, sizeof(desc),
                         "温度异常：%.2f ℃", snapshot.temperature);
                generate_alarm(snapshot.device_id, 2, 2, desc);
                break;
            case 3:
                snprintf(desc, sizeof(desc),
                         "电流异常：%.2f A", snapshot.current);
                generate_alarm(snapshot.device_id, 2, 3, desc);
                break;
            }
        }

        sleep(3);
    }

    printf("[device_monitor] Thread exiting\n");
    return NULL;
}
