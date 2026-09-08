/*
 * public.h - 通信协议数据结构定义
 *
 * ===== 协议设计 =====
 *
 * 通信双方（client ↔ server）严格按照固定格式收发数据：
 *
 *   [packet_head: 8 字节] [payload: head.size 字节]
 *
 *   packet_head:
 *     agreement_id (4B) - 协议 ID，标识这个消息是什么类型
 *     size          (4B) - payload 的字节数
 *
 * 协议 ID 一览表：
 *   0x00 - 设备注册鉴权请求 ← client → server
 *   0x01 - 状态变化上报     ← client → server（状态变了才发）
 *   0x02 - 重量变化上报     ← client → server（原项目遗留）
 *   0x03 - 传感器数据上报   ← client → server（周期性上报）
 *   0x10 - 控制命令         ← server → client
 *   0x20 - 报警信息         ← client → server
 *
 * 为什么用二进制协议而不是 JSON/文本协议？
 *   1. 嵌入式设备资源有限，二进制解析快、带宽小
 *   2. ARM Cortex-A7 上序列化/反序列化 JSON 成本高
 *   3. 结构体可以直接 memcpy 收发，省 CPU
 *
 * 注意事项（跨平台）：
 *   结构体含 padding 字节，client 和 server 必须用相同的编译选项。
 *   本例中两端都是 Linux/gcc，自然对齐一致。
 *   如果涉及不同架构通信，需要添加 __attribute__((packed))。
 */

#ifndef PUBLIC_H
#define PUBLIC_H

#include <time.h>

/* =================================================================
 * [packet_head] - 所有网络数据包的公共头部
 * =================================================================
 * 不管发送什么数据，前面都得先发这 8 个字节告诉对方：
 * - "这是什么类型的消息"（agreement_id）
 * - "后面跟了多少数据"（size）
 */
typedef struct head {
    int agreement_id;    // 协议 ID（0x00 ~ 0x20）
    int size;            // 后面 payload 的字节数
} packet_head;

/* =================================================================
 * [0x00] 设备注册鉴权请求
 * =================================================================
 * client 连接上 server 后，第一条消息必须是设备注册。
 * 相当于"你好，我是设备 #1，类型是 Motor，现在时间是 xxx"
 * server 验证后会回复注册结果。
 */
typedef struct device_logon {
    unsigned int device_id;       // 设备 ID（唯一标识）
    time_t timestamp;             // 时间戳（告知服务器当前设备时间）
    char device_type[32];         // 设备类型：电机、泵、风机等
} device_logon_t;

/* =================================================================
 * [0x01/0x02/0x03] 设备状态信息
 * =================================================================
 * 这是最核心的数据结构，包含一个工业设备的全部运行参数：
 * - 振动三轴 + 总振动（判断机械故障的重要指标）
 * - 温湿度（监测运行环境）
 * - 电流/电压/功率（电气参数）
 * - 转速（旋转设备的关键指标）
 * - 故障码和维护时间（预维护用）
 */
typedef struct device_status {
    unsigned int device_id;       // 设备 ID
    char device_type[32];         // 设备类型
    short run_state;              // 运行状态：0-停止 1-运行 2-故障
    float temperature;            // 温度（℃）
    float humidity;               // 湿度（%RH）
    float vibration_x;            // X 轴振动值（m/s²）
    float vibration_y;            // Y 轴振动值（m/s²）
    float vibration_z;            // Z 轴振动值（m/s²）
    float vibration_total;        // 总振动瞬时值（m/s²）= sqrt(x² + y² + z²)
    float vibration_rms;          // 振动 RMS（滑动窗口 1 秒，m/s²）← 边缘计算核心指标
    float vibration_peak;         // 振动 Peak（窗口内最大值，m/s²）   ← 检测冲击性故障
    float rpm;                    // 转速（RPM）
    float current;                // 电流（A）
    float voltage;                // 电压（V）
    float power;                  // 功率（kW）
    unsigned int fault_code;      // 故障码（0 = 无故障）
    time_t last_maintenance;      // 上次维护时间
    time_t next_maintenance;      // 下次维护时间（预维护用）
} device_status_t;

/* =================================================================
 * [0x10] 控制命令
 * =================================================================
 * 服务器远程控制设备：启动、停止、复位、设置转速。
 * 这是工业场景的必备功能——不需要人到现场操作。
 */
typedef struct control_cmd {
    unsigned int device_id;       // 目标设备 ID
    unsigned char cmd_type;       // 命令类型：0-启动 1-停止 2-复位
    unsigned char reserved[3];    // 保留，对齐用
    float set_rpm;                // 设定转速（仅 cmd_type=0 时有效）
} control_cmd_t;

/* =================================================================
 * [0x20] 报警信息
 * =================================================================
 * 当监测到异常时（振动超阈值、温度过高、电流过大），
 * client 主动向 server 发送报警消息。
 * 报警有三个级别：1-警告 2-严重 3-紧急
 */
typedef struct alarm_info {
    unsigned int device_id;       // 设备 ID
    unsigned int alarm_id;        // 报警 ID（用于去重）
    unsigned char alarm_level;    // 报警级别：1-警告 2-严重 3-紧急
    unsigned char alarm_type;     // 报警类型：1-振动 2-温度 3-电流 4-其他
    time_t alarm_time;            // 报警发生时间
    char alarm_desc[64];          // 报警描述文本
} alarm_info_t;

#endif
