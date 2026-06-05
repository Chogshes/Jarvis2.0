// device.c —— 智能设备模拟进程 + 链表管理
//
// 架构: 每个设备 fork 一个子进程, 用双向管道 IPC 通信
//   父进程 → cmd_fd → 子进程 (ON/OFF/SET/STATUS/QUIT)
//   父进程 ← resp_fd ← 子进程 (STATE <on_off> <value>)
//   子进程模拟设备行为: 温度漂移、设备状态变化等

#include "device.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/wait.h>

// ═══════════════════════════════════════════════════════════
//  设备子进程逻辑
// ═══════════════════════════════════════════════════════════

// 每种设备的模拟行为: 读命令 → 更新状态 → 写响应
static void device_simulator(int cmd_fd, int resp_fd, DeviceType type)
{
    int state = 0;   // 默认关
    int value = 0;

    // 不同类型默认值
    switch (type) {
    case DEV_LIGHT:        value = 80;  break;  // 默认亮度
    case DEV_THERMOSTAT:   value = 250; break;  // 25.0°C
    case DEV_REFRIGERATOR: value = 50;  break;  // 5.0°C
    case DEV_WASHER:       value = 0;   break;  // 空闲
    case DEV_CURTAIN:      value = 100; break;  // 全开
    case DEV_AIR_PURIFIER: value = 1;   break;  // 低风
    case DEV_ARC:          value = 250; break;  // 设定 25°C
    default: break;
    }

    char buf[128];
    int tick = 0;

    while (1) {
        // 非阻塞读命令
        int n = read(cmd_fd, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';

            // 去掉换行
            char *nl = strchr(buf, '\n');
            if (nl) *nl = '\0';
            if (nl && nl > buf && *(nl-1) == '\r') *(nl-1) = '\0';

            if (strncmp(buf, "ON", 2) == 0) {
                state = 1;
            } else if (strncmp(buf, "OFF", 3) == 0) {
                state = 0;
            } else if (strncmp(buf, "SET ", 4) == 0) {
                value = atoi(buf + 4);
            } else if (strncmp(buf, "STATUS", 6) == 0) {
                // 立即返回状态
                char resp[64];
                snprintf(resp, sizeof(resp), "STATE %d %d\n", state, value);
                write(resp_fd, resp, strlen(resp));
            } else if (strncmp(buf, "QUIT", 4) == 0) {
                break;
            }
        } else if (n < 0 && errno != EAGAIN) {
            break;  // pipe broken
        }

        // 模拟传感器漂移 (温度计/冰箱随时间变化)
        if (type == DEV_THERMOSTAT) {
            if (++tick % 50 == 0) {  // 约每 5 秒
                value += (rand() % 3) - 1;  // ±1 (0.1°C)
                // 保持合理范围 15~35°C
                if (value < 150) value = 150;
                if (value > 350) value = 350;
            }
        }
        if (type == DEV_REFRIGERATOR && state) {
            if (++tick % 80 == 0) {
                value -= 1;  // 制冷降温
                if (value < 20) value = 20;  // 最低 2°C
            }
        }
        if (type == DEV_REFRIGERATOR && !state) {
            if (++tick % 100 == 0) {
                value += 1;  // 关机制冷回升
                if (value > 120) value = 120;  // 最高 12°C
            }
        }

        // 状态变化时上报
        if (tick % 20 == 0) {  // 约每 2 秒
            char resp[64];
            snprintf(resp, sizeof(resp), "STATE %d %d\n", state, value);
            write(resp_fd, resp, strlen(resp));
        }

        usleep(100000);  // 100ms 轮询周期
    }

    close(cmd_fd);
    close(resp_fd);
    _exit(0);
}

// ═══════════════════════════════════════════════════════════
//  设备管理器链表操作
// ═══════════════════════════════════════════════════════════

void dev_mgr_init(DeviceManager *mgr)
{
    mgr->head    = NULL;
    mgr->count   = 0;
    mgr->next_id = 1;
}

Device *dev_mgr_add(DeviceManager *mgr, const char *name, DeviceType type)
{
    Device *dev = calloc(1, sizeof(Device));
    if (!dev) return NULL;

    dev->id    = mgr->next_id++;
    dev->type  = type;
    dev->state = 0;
    strncpy(dev->name, name, sizeof(dev->name) - 1);

    // 创建两个管道
    int cmd_pipe[2];   // parent 写 → child 读
    int resp_pipe[2];  // child 写 → parent 读

    if (pipe(cmd_pipe) < 0 || pipe(resp_pipe) < 0) {
        free(dev); return NULL;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(cmd_pipe[0]); close(cmd_pipe[1]);
        close(resp_pipe[0]); close(resp_pipe[1]);
        free(dev); return NULL;
    }

    if (pid == 0) {
        // 子进程
        close(cmd_pipe[1]);   // 关闭写端，保留读端
        close(resp_pipe[0]);  // 关闭读端，保留写端

        // cmd_fd 设为非阻塞
        int flags = fcntl(cmd_pipe[0], F_GETFL);
        fcntl(cmd_pipe[0], F_SETFL, flags | O_NONBLOCK);

        device_simulator(cmd_pipe[0], resp_pipe[1], type);
        // 不会到达这里
    }

    // 父进程
    close(cmd_pipe[0]);   // 关闭读端，保留写端
    close(resp_pipe[1]);  // 关闭写端，保留读端

    // resp_fd 设为非阻塞
    int flags = fcntl(resp_pipe[0], F_GETFL);
    fcntl(resp_pipe[0], F_SETFL, flags | O_NONBLOCK);

    dev->pid     = pid;
    dev->cmd_fd  = cmd_pipe[1];
    dev->resp_fd = resp_pipe[0];

    // 链表头插入
    dev->next = mgr->head;
    mgr->head = dev;
    mgr->count++;

    fprintf(stderr, "[设备] 添加 id=%d name=%s type=%d pid=%d\n",
            dev->id, dev->name, dev->type, dev->pid);
    fflush(stderr);

    return dev;
}

void dev_mgr_remove(DeviceManager *mgr, int id)
{
    Device **pp = &mgr->head;
    while (*pp) {
        Device *dev = *pp;
        if (dev->id == id) {
            dev_send_command(dev, "QUIT");
            close(dev->cmd_fd);
            close(dev->resp_fd);
            waitpid(dev->pid, NULL, 0);
            *pp = dev->next;
            free(dev);
            mgr->count--;
            return;
        }
        pp = &(*pp)->next;
    }
}

Device *dev_mgr_find_by_id(DeviceManager *mgr, int id)
{
    dev_foreach(mgr, dev)
        if (dev->id == id) return dev;
    return NULL;
}

Device *dev_mgr_find_by_type(DeviceManager *mgr, DeviceType type)
{
    dev_foreach(mgr, dev)
        if (dev->type == type) return dev;
    return NULL;
}

void dev_send_command(Device *dev, const char *cmd)
{
    if (!dev || dev->cmd_fd < 0) return;
    char buf[64];
    snprintf(buf, sizeof(buf), "%s\n", cmd);
    write(dev->cmd_fd, buf, strlen(buf));
}

// ── 轮询设备状态 ─────────────────────────────────────────
void dev_mgr_poll(DeviceManager *mgr)
{
    dev_foreach(mgr, dev) {
        if (dev->resp_fd < 0) continue;
        char buf[128];
        int n = read(dev->resp_fd, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            // 解析 "STATE <on_off> <value>"
            int s = 0, v = 0;
            if (sscanf(buf, "STATE %d %d", &s, &v) >= 2) {
                dev->state = s;
                dev->value = v;
            }
        }
    }
}

void dev_mgr_shutdown(DeviceManager *mgr)
{
    dev_foreach(mgr, dev) {
        dev_send_command(dev, "QUIT");
        close(dev->cmd_fd);
        close(dev->resp_fd);
        waitpid(dev->pid, NULL, 0);
    }
    // 释放链表
    Device *dev = mgr->head;
    while (dev) {
        Device *next = dev->next;
        free(dev);
        dev = next;
    }
    mgr->head  = NULL;
    mgr->count = 0;
}
