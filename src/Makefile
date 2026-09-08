# Makefile - 构建系统
#
# 支持两套编译目标：
#   client_arm  - ARM 交叉编译（i.MX6ULL 目标）
#   server      - 本地 gcc 编译（PC 运行）
#
# 使用说明：
#   make              - 编译 client_arm + server
#   make client_arm   - 只编译 ARM 客户端
#   make server       - 只编译 PC 服务器
#   make clean        - 清理产物
#
# ===== 关于 LVGL =====
# client_arm 依赖 LVGL 库（预编译 .o 文件在 lv_port_linux-release-v8.4/）。
# 如果需要 GUI 功能，需要先编译 LVGL，并将 lv_port_linux-release-v8.4 目录
# 放到项目根目录下，然后在 CFLAGS 中添加 -I 路径，LDFLAGS 中添加 .o 文件。
#
# 当前 Makefile 编译的是无 GUI 版本，保持最小依赖。
# 启用 GUI 的完整编译参见 build_with_lvgl.sh

CC = arm-linux-gnueabihf-gcc
SERVER_CC = gcc
CFLAGS = -Wall -pthread -I.
LDFLAGS = -lm -lsqlite3

# client 源文件（不含 lqueue.c、gui.c、trash.c，它们已废弃/可选）
# storage.c 是 SQLite 历史数据存储模块，需要 -lsqlite3 链接
SRC = main.c network.c industrial.c sensor.c mailbox.c linkque.c storage.c

# 目标文件
OBJ = $(SRC:.c=.o)

# 最终产物
CLIENT_TARGET = client_arm
SERVER_TARGET = server

# ===== 默认目标 =====
all: $(CLIENT_TARGET) $(SERVER_TARGET)

# ===== client_arm：ARM 交叉编译 =====
$(CLIENT_TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# ===== server：本地 gcc 编译 =====
$(SERVER_TARGET): server.c
	$(SERVER_CC) $(CFLAGS) -o $@ $^

# ===== 隐式规则：.c → .o =====
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# ===== 清理 =====
clean:
	rm -f $(OBJ) $(CLIENT_TARGET) $(SERVER_TARGET)

.PHONY: all clean
