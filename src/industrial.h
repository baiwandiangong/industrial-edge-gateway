/*
 * industrial.h - 工业设备业务逻辑
 *
 * ===== 职责分离 =====
 *
 * sensor.c 负责"怎么读"——和硬件打交道
 * industrial.c 负责"读出值后怎么做"——业务规则
 *
 * 具体职责：
 * 1. 设备状态监测（轮询 device_info，检测状态变化）
 * 2. 报警条件判断（振动超限、温度超高、电流过大）
 * 3. 生成/发送报警消息
 *
 * 为什么把报警逻辑单独放在这里而不是 sensor 里？
 * 因为报警规则是业务相关的，换一种设备（比如从电机换成泵），
 * 报警阈值和规则可能完全不同。单独放便于修改和维护。
 */

#ifndef INDUSTRIAL_H
#define INDUSTRIAL_H

#include <stdio.h>
#include <unistd.h>
#include "public.h"

/*
 * 设备状态监测线程
 * 每 3 秒检查 device_info 是否有显著变化，
 * 有变化则通过 mailbox 发给 net_sender 上报服务器。
 * 同时检查报警条件。
 *
 * 为什么是 3 秒而不是和 sensor 一样的 10ms？
 * - 状态变化上报不需要高频（状态变了才报）
 * - 设备运行的正常状态变化是慢变的
 * - 报警检测有实时性要求但 3 秒足够
 */
extern void *device_monitor_thread(void *arg);

/*
 * 检查报警条件
 * 返回 0 = 正常，1 = 振动异常，2 = 温度异常，3 = 电流异常
 */
extern int check_alarm_condition(device_status_t *status);

/*
 * 生成并发送报警信息
 * 通过 mailbox 发给 net_sender，由 net_sender 发往服务器
 */
extern void generate_alarm(unsigned int device_id,
                           unsigned char level,
                           unsigned char type,
                           const char *desc);

#endif
