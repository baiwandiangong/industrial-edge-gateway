/*
 * sensor.c - 传感器数据采集实现
 *
 * ===== 改进：统一走 mailbox =====
 *
 * 原项目中，sensor_collect_thread 采集到数据后，直接 send_packet(sockfd) 写 socket。
 * 这是一个架构缺陷——每个线程都可能写 socket，需要用 sockfd_mutex 保护。
 *
 * 改进方案：
 *   sensor_collect_thread 采集数据后，把数据封装成 MAIL_DATA，
 *   通过 send_msg(mbs, "net_sender", &mail) 发给 net_sender 线程，
 *   由 net_sender 统一写 socket。
 *
 * 好处：
 *   - 只有一个线程写 socket，不需要 sockfd_mutex
 *   - net_sender 可以在发送前统一做流量控制、数据加密等
 *   - send_packet 调用集中在 net_sender 中，便于调试
 */

#include "sensor.h"
#include "mailbox.h"
#include "public.h"
#include "network.h"
#include "main.h"

/* 外部全局变量声明 */
extern MBS *g_mbs;
extern device_status_t device_info;
extern pthread_mutex_t device_info_mutex;
extern int sockfd;

/* =================================================================
 * RMS / Peak 滑动窗口（边缘计算核心）
 * =================================================================
 * 为什么用滑动窗口？
 *   振动是连续信号，单次瞬时值无法判断设备状态。
 *   ISO 10816 标准规定用 1 秒内的 RMS 评估振动等级。
 *
 * 窗口大小：100 个采样 = 1 秒（10ms × 100）
 * 算法：循环 buffer，每来一个新值就更新一次窗口
 *
 * 为什么不每 10ms 全量遍历 100 个数算一次？
 *   - 全量遍历 O(n) × 100Hz = 10000 次/秒，对单核 ARM 有压力
 *   - 增量算法 O(1)：维护一个 sum_sq，减去最老的，加上最新的
 *
 * 简化版：本项目用全量遍历，100 个数 × 100Hz × 单次 mul+add ≈ 0.1ms，
 *        对 i.MX6ULL 单核 CPU 完全可接受，且代码更易读。
 * ================================================================= */
#define RMS_WINDOW_SIZE 100
static float  g_vib_window[RMS_WINDOW_SIZE] = {0};
static int    g_vib_idx = 0;
static int    g_vib_filled = 0;          /* 已填充的样本数（用于前几次不满窗口时）*/

static int adxl345_fd = -1;
static int sht30_fd = -1;

/*
 * sensor_init - 打开传感器设备文件
 *
 * 传感器驱动在内核中注册，提供字符设备接口。
 * 如果某个传感器不存在，adxl345_fd/sht30_fd 保持 -1，
 * 后续读取函数会跳过，返回 0。
 *
 * 这种"容错初始化"模式在嵌入式系统中很常见：
 * 硬件不是每个板子都完全一致，软件应该能优雅降级。
 */
void sensor_init(void)
{
    printf("[sensor] Initializing ADXL345...\n");
    adxl345_fd = open("/dev/adxl345", O_RDWR);
    if (adxl345_fd < 0)
    {
        perror("[sensor] open adxl345 failed");
        printf("[sensor] ADXL345 not available (vibration data will be 0)\n");
        adxl345_fd = -1;
    }
    else
    {
        printf("[sensor] ADXL345 opened successfully\n");
    }

    printf("[sensor] Initializing SHT30...\n");
    sht30_fd = open("/dev/sht30", O_RDONLY);
    if (sht30_fd < 0)
    {
        perror("[sensor] open sht30 failed");
        printf("[sensor] SHT30 not available (temp/humid data will be 0)\n");
        sht30_fd = -1;
    }
    else
    {
        printf("[sensor] SHT30 opened successfully\n");
    }
}

/*
 * sensor_read_vibration - 读取 ADXL345 三轴振动
 *
 * ADXL345 原始数据是 16 位有符号整数（short），
 * 在 ±2g 量程下，灵敏度约 3.9mg/LSB。
 *
 * 转换公式：m/s² = raw × 0.0039 × 9.8
 * 0.0039 = 3.9mg / 1000（转为 g）
 * 9.8 = g 常量（转为 m/s²）
 */
void sensor_read_vibration(float *x, float *y, float *z)
{
    if (adxl345_fd < 0)
    {
        *x = *y = *z = 0;
        return;
    }

    short data[3] = {0};
    int ret = read(adxl345_fd, data, sizeof(data));
    if (ret > 0)
    {
        *x = data[0] * 0.0039f * 9.8f;
        *y = data[1] * 0.0039f * 9.8f;
        *z = data[2] * 0.0039f * 9.8f;
    }
    else
    {
        *x = *y = *z = 0;
    }
}

/*
 * sensor_read_humidity_temperature - 读取 SHT30 温湿度
 *
 * 驱动层将原始值 × 100 后返回整数，用户态除以 100 得实际值。
 * 例如：温度 25.5℃ → 驱动返回 2550 → 用户态 2550/100 = 25.5
 */
void sensor_read_humidity_temperature(float *temperature, float *humidity)
{
    if (sht30_fd < 0)
    {
        *temperature = 0;
        *humidity = 0;
        return;
    }

    struct sht30_data {
        short temperature_raw;
        short humidity_raw;
    } data;
    ssize_t ret = read(sht30_fd, &data, sizeof(data));
    if (ret == sizeof(data))
    {
        *temperature = data.temperature_raw / 100.0f;
        *humidity = data.humidity_raw / 100.0f;
    }
    else
    {
        *temperature = 0;
        *humidity = 0;
    }
}

/*
 * sensor_collect_thread - 传感器数据采集线程
 *
 * ===== 工作流 =====
 *
 * 1. 初始化传感器（打开设备文件）
 * 2. 循环（每 10ms）：
 *    a. 读取振动三轴 + 计算总振动
 *    b. 读取温湿度
 *    c. 更新 device_info（mutex 保护）
 *    d. 封装数据到 MAIL_DATA
 *    e. send_msg 发给 net_sender 线程（协议 0x03，实时上报）
 *    f. 每 100 次采样（约 1 秒）再投递一份快照给 storage 线程写 SQLite
 *
 * 注意：
 *   - 采集频率 100Hz（10ms）对于工业振动监测足够了
 *   - 如果传感器不可用，数据为 0，但程序继续运行
 *   - 不直接操作 socket，所有数据通过 mailbox 中转
 */
void *sensor_collect_thread(void *arg)
{
    (void)arg;
    printf("[sensor_collect] Thread started\n");

    sensor_init();

    if (adxl345_fd < 0 && sht30_fd < 0)
    {
        printf("[sensor_collect] No sensors available, will simulate data\n");
        /* 不退出，继续运行——没有硬件时可以模拟数据用于测试 */
    }

    int save_cnt = 0;  /* 落库节流：每 100 次采样（1 秒）投递一条给 storage */
    while (1)
    {
        /* === (a) 读取振动 === */
        float vib_x, vib_y, vib_z;
        sensor_read_vibration(&vib_x, &vib_y, &vib_z);

        /* 总振动 = sqrt(x² + y² + z²) */
        float vib_total = sqrtf(vib_x * vib_x +
                                vib_y * vib_y +
                                vib_z * vib_z);

        /* === (a.5) RMS / Peak 滑动窗口（边缘计算）===
         *
         * 关键设计：
         *   1. 循环 buffer（index 取模）
         *   2. 旧值自动覆盖（窗口满了之后）
         *   3. RMS = sqrt(Σxi² / N)
         *   4. Peak = max(x0..xN-1)
         *
         * 注意：前三行注释这里用全量遍历而非增量算法，因为：
         *   - 单核 ARM @ 528MHz 处理 100 个浮点平方 ≈ 30us
         *   - 增量算法需要保存"被覆盖的旧值"，逻辑反而复杂
         *   - 简单可读优先（嵌入式场景下维护性 > 微秒级优化）
         */
        g_vib_window[g_vib_idx] = vib_total;
        g_vib_idx = (g_vib_idx + 1) % RMS_WINDOW_SIZE;
        if (g_vib_filled < RMS_WINDOW_SIZE) g_vib_filled++;

        /* 计算窗口内 RMS 和 Peak */
        float sq_sum = 0.0f;
        float peak = 0.0f;
        for (int i = 0; i < g_vib_filled; i++) {
            float v = g_vib_window[i];
            sq_sum += v * v;
            if (v > peak) peak = v;
        }
        float vib_rms  = sqrtf(sq_sum / g_vib_filled);
        float vib_peak = peak;

        /* === (b) 读取温湿度 === */
        float temperature, humidity;
        sensor_read_humidity_temperature(&temperature, &humidity);

        /* === (c) 更新设备状态 === */
        pthread_mutex_lock(&device_info_mutex);
        device_info.vibration_x = vib_x;
        device_info.vibration_y = vib_y;
        device_info.vibration_z = vib_z;
        device_info.vibration_total = vib_total;
        device_info.vibration_rms  = vib_rms;     /* ← 新增：滑动窗口 RMS */
        device_info.vibration_peak = vib_peak;    /* ← 新增：窗口内峰值 */
        device_info.temperature = temperature;
        device_info.humidity = humidity;
        pthread_mutex_unlock(&device_info_mutex);

        /* === (d) 通过 mailbox 发送到 net_sender ===
         *
         * 注意：这里发送的是协议 0x03（传感器数据上报）。
         * 但采集频率是 10ms（100Hz），服务器可能扛不住这么高的频率。
         * 在实际项目中，这里会做一个降频处理，比如每 100ms 发一次，
         * 或者只发变化超过阈值的数据。
         * 目前为了演示架构，保持完整上报。
         */
        device_status_t snapshot;
        pthread_mutex_lock(&device_info_mutex);
        memcpy(&snapshot, &device_info, sizeof(snapshot));
        pthread_mutex_unlock(&device_info_mutex);

        send_to_net_sender(0x03, &snapshot, sizeof(snapshot));
        /* === (d.2) 本地历史存储：经 mailbox 投递给 storage 线程 ===
         *
         * 100Hz 采集全量落库没有必要（每秒 100 行会放大磁盘 IO），
         * 这里每 100 次采样（= 1 秒）封装一份最新快照发给 "storage"，
         * 由 storage 线程写入 SQLite。存储与网络上报共用同一套 mailbox，
         * storage 不需要自己轮询 device_info，生产节奏完全由本线程控制。
         */
        if (++save_cnt >= 100)
        {
            save_cnt = 0;

            MAIL_DATA smail;
            memset(&smail, 0, sizeof(smail));
            memcpy(smail.data, &snapshot, sizeof(snapshot));
            smail.payload_len = (int)sizeof(snapshot);

            if (send_msg(g_mbs, "storage", &smail) != 0)
            {
                printf("[sensor] send to storage failed\n");
            }
        }

        /* === (e) 日志输出 === */
        printf("[sensor] X=%.2f Y=%.2f Z=%.2f | Total=%.2f RMS=%.2f Peak=%.2f m/s² | "
               "Temp=%.1f°C Humid=%.1f%%\n",
               vib_x, vib_y, vib_z, vib_total, vib_rms, vib_peak,
               temperature, humidity);

        /* 10ms 采样间隔 */
        usleep(10 * 1000);
    }

    /* 清理 */
    if (adxl345_fd >= 0) close(adxl345_fd);
    if (sht30_fd >= 0)   close(sht30_fd);

    printf("[sensor_collect] Thread exiting\n");
    return NULL;
}
