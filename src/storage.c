/*
 * storage.c - SQLite3 历史数据存储实现
 *
 * 表结构：
 *   sensor_history(
 *     id INTEGER PRIMARY KEY AUTOINCREMENT,
 *     ts INTEGER NOT NULL,             -- Unix 时间戳（秒）
 *     vib_rms REAL,                    -- 振动 RMS（m/s²）
 *     vib_peak REAL,                   -- 振动峰值（m/s²）
 *     temperature REAL,                -- 温度（℃）
 *     humidity REAL,                   -- 湿度（%RH）
 *     uploaded INTEGER DEFAULT 0      -- 0=未上报 1=已上报（用于补传）
 *   )
 *
 * 设计要点：
 *   1. 数据经 Mailbox 消息总线驱动：sensor_collect 每 1 秒投递一条最新快照
 *   2. uploaded 字段支持断网补传——断网时本地累积，恢复后批量上报
 *   3. 用 sqlite3_bind_* 参数化查询，防止 SQL 注入
 *   4. 每秒落库 1 行（100Hz 采集降频到 1Hz 写库），不按 100Hz 每帧写，降低 IO 压力
 *
 * 编译依赖：链接时加 -lsqlite3
 *   arm-linux-gnueabihf-gcc ... -lsqlite3
 */

#include "storage.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include "main.h"
#include "mailbox.h"

static sqlite3 *g_db = NULL;
static pthread_mutex_t g_db_mutex = PTHREAD_MUTEX_INITIALIZER;

/*
 * storage_init - 打开/创建数据库，建表
 * 返回 0 成功，-1 失败
 */
int storage_init(void)
{
    int rc = sqlite3_open("/tmp/sensor_history.db", &g_db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[storage] 无法打开数据库: %s\n", sqlite3_errmsg(g_db));
        return -1;
    }

    const char *sql =
        "CREATE TABLE IF NOT EXISTS sensor_history ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  ts INTEGER NOT NULL,"
        "  vib_rms REAL,"
        "  vib_peak REAL,"
        "  temperature REAL,"
        "  humidity REAL,"
        "  uploaded INTEGER DEFAULT 0"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_ts ON sensor_history(ts);"
        "CREATE INDEX IF NOT EXISTS idx_uploaded ON sensor_history(uploaded);";

    char *errmsg = NULL;
    rc = sqlite3_exec(g_db, sql, NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[storage] 建表失败: %s\n", errmsg);
        sqlite3_free(errmsg);
        sqlite3_close(g_db);
        g_db = NULL;
        return -1;
    }

    printf("[storage] 数据库初始化成功: /tmp/sensor_history.db\n");
    return 0;
}

/*
 * storage_save - 插入一条传感器记录
 * 参数 status: 当前设备状态
 * 返回 0 成功，-1 失败
 *
 * 注意：
 *   - 这是高频调用，要尽量快
 *   - 用参数化绑定（sqlite3_bind_*）而不是字符串拼接
 */
int storage_save(const device_status_t *status)
{
    if (!g_db || !status) return -1;

    pthread_mutex_lock(&g_db_mutex);

    const char *sql =
        "INSERT INTO sensor_history"
        "  (ts, vib_rms, vib_peak, temperature, humidity, uploaded)"
        " VALUES (?, ?, ?, ?, ?, 0);";

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[storage] prepare 失败: %s\n", sqlite3_errmsg(g_db));
        pthread_mutex_unlock(&g_db_mutex);
        return -1;
    }

    time_t now = time(NULL);
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)now);
    sqlite3_bind_double(stmt, 2, status->vibration_rms);
    sqlite3_bind_double(stmt, 3, status->vibration_peak);
    sqlite3_bind_double(stmt, 4, status->temperature);
    sqlite3_bind_double(stmt, 5, status->humidity);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    pthread_mutex_unlock(&g_db_mutex);

    if (rc != SQLITE_DONE) {
        fprintf(stderr, "[storage] insert 失败: %s\n", sqlite3_errmsg(g_db));
        return -1;
    }
    return 0;
}

/*
 * storage_recent - 查询最近 N 条记录，返回 JSON 字符串
 * 调用者负责 free()
 *
 * 为什么返回 JSON？
 *   简化解析——server.c 直接打印或转发给前端
 *   如果直接返回结构体数组，需要调用方定义解析逻辑
 */
char *storage_recent(int n)
{
    if (!g_db) return NULL;

    pthread_mutex_lock(&g_db_mutex);

    const char *sql =
        "SELECT id, ts, vib_rms, vib_peak, temperature, humidity"
        " FROM sensor_history ORDER BY id DESC LIMIT ?;";

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        pthread_mutex_unlock(&g_db_mutex);
        return NULL;
    }
    sqlite3_bind_int(stmt, 1, n);

    /* 估算 buffer 大小：每条约 100 字节 */
    int buf_size = n * 100 + 32;
    char *buf = (char *)malloc(buf_size);
    if (!buf) {
        sqlite3_finalize(stmt);
        pthread_mutex_unlock(&g_db_mutex);
        return NULL;
    }
    int offset = 0;
    offset += snprintf(buf + offset, buf_size - offset, "[");

    int count = 0;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (count > 0) offset += snprintf(buf + offset, buf_size - offset, ",");
        offset += snprintf(buf + offset, buf_size - offset,
            "{\"id\":%lld,\"ts\":%lld,\"rms\":%.3f,\"peak\":%.3f,"
            "\"temp\":%.2f,\"humi\":%.2f}",
            (long long)sqlite3_column_int64(stmt, 0),
            (long long)sqlite3_column_int64(stmt, 1),
            sqlite3_column_double(stmt, 2),
            sqlite3_column_double(stmt, 3),
            sqlite3_column_double(stmt, 4),
            sqlite3_column_double(stmt, 5));
        count++;
    }

    offset += snprintf(buf + offset, buf_size - offset, "]");

    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_db_mutex);

    return buf;
}

/*
 * storage_pending_count - 查询未上报条数（断网补传用）
 */
int storage_pending_count(void)
{
    if (!g_db) return 0;

    pthread_mutex_lock(&g_db_mutex);

    const char *sql = "SELECT COUNT(*) FROM sensor_history WHERE uploaded = 0;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        pthread_mutex_unlock(&g_db_mutex);
        return 0;
    }

    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_db_mutex);
    return count;
}

/*
 * storage_mark_uploaded - 标记记录已上报
 * 用于断网恢复后批量补传
 */
int storage_mark_uploaded(long long id)
{
    if (!g_db) return -1;

    pthread_mutex_lock(&g_db_mutex);

    const char *sql = "UPDATE sensor_history SET uploaded = 1 WHERE id = ?;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        pthread_mutex_unlock(&g_db_mutex);
        return -1;
    }
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)id);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&g_db_mutex);

    return (rc == SQLITE_DONE) ? 0 : -1;
}

/*
 * storage_close - 关闭数据库
 */
void storage_close(void)
{
    if (g_db) {
        sqlite3_close(g_db);
        g_db = NULL;
        printf("[storage] 数据库已关闭\n");
    }
}

/*
 * storage_thread - 存储线程入口（消息驱动）
 *
 * 行为：
 *   - 线程启动时调用 storage_init() 打开/创建数据库并建表
 *   - 循环 recv_msg() 收 mailbox 消息（非阻塞，无消息则休眠 10ms 重试）
 *   - 收到 sensor_collect 投递的 device_status_t 快照 → 长度校验 → storage_save()
 *
 * 为什么用消息驱动而不是自己定时轮询 device_info？
 *   - 生产节奏由 sensor_collect 决定，storage 只被动消费，职责单一
 *   - 和 net_sender / net_parse 共用同一套 mailbox 机制，架构统一
 *   - 以后想改"阈值落库/变化落库"，只动发送端，接收端零改动
 */

void *storage_thread(void *arg)
{
    (void)arg;

    /* 打开数据库并建表（CREATE TABLE IF NOT EXISTS） */
    if (storage_init() != 0) {
        return NULL;
    }

    while (1) {
        MAIL_DATA mail;
        memset(&mail, 0, sizeof(mail));

        /* 非阻塞收信：没有消息就睡 10ms 再试 */
        if (recv_msg(g_mbs, &mail) != 0) {
            usleep(10 * 1000);
            continue;
        }

        /* 载荷必须是完整的 device_status_t 快照，否则丢弃 */
        if (mail.payload_len != (int)sizeof(device_status_t)) {
            fprintf(stderr, "[storage] 非法消息长度: %d (期望 %d)\n",
                    mail.payload_len, (int)sizeof(device_status_t));
            continue;
        }

        /* 取出快照写入 SQLite（sensor_history 表） */
        device_status_t snapshot;
        memcpy(&snapshot, mail.data, sizeof(snapshot));
        storage_save(&snapshot);
    }

    storage_close();
    return NULL;
}
