/*
 * sensor.h - 传感器数据采集
 *
 * ===== 传感器硬件 =====
 *
 * 本项目使用两种传感器：
 *
 * ADXL345 - 三轴加速度传感器（SPI 接口）
 *   用于测量设备振动：X/Y/Z 三轴加速度
 *   量程 ±2g，分辨率 3.9mg/LSB
 *   测量值经简单换算 = raw * 0.0039 * 9.8 → m/s²
 *   通过 /dev/adxl345 字符设备读取
 *
 * SHT30 - 温湿度传感器（I2C 接口）
 *   用于测量环境温湿度
 *   通过 /dev/sht30 字符设备读取
 *
 * ===== 采集策略 =====
 *
 * 采集频率：10ms 一次（100Hz），这是一个典型的工业振动采样频率。
 * 为什么是 10ms？
 *   工业设备振动通常 10~1000Hz，100Hz 的采样率可以捕捉到主要振动特征。
 *   更高的采样率意味着更多的 CPU 开销，10ms 是一个合理的折中。
 *
 * 采集到数据后：
 *   1. 更新 device_info 全局结构体（mutex 保护）
 *   2. 通过 mailbox 发给 "net_sender" 线程统一发送
 *   3. 本线程不碰 socket
 */

#ifndef SENSOR_H
#define SENSOR_H

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <math.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>
#include "public.h"

/* 传感器初始化（打开 /dev/adxl345 和 /dev/sht30） */
extern void sensor_init(void);

/* 读取三轴振动值（ADXL345） */
extern void sensor_read_vibration(float *x, float *y, float *z);

/* 读取温湿度（SHT30） */
extern void sensor_read_humidity_temperature(float *temperature, float *humidity);

/*
 * 传感器数据采集线程
 * - 被 main.c 注册到 mailbox 系统
 * - 10ms 间隔采集数据
 * - 更新 device_info
 * - 数据通过 mailbox 发往 net_sender 线程
 */
extern void *sensor_collect_thread(void *arg);

#endif
