#ifndef DEVICE_H
#define DEVICE_H

#ifdef __cplusplus
extern "C" {
#endif

// ── Device types ─────────────────────────────────────────
typedef enum {
    DEV_LIGHT,          // 灯 (value = 亮度 0-100)
    DEV_THERMOSTAT,     // 温度计 (value = 温度 °C * 10)
    DEV_REFRIGERATOR,   // 冰箱 (value = 温度 °C * 10)
    DEV_WASHER,         // 洗衣机 (value = 运行模式 0=空闲 1=洗涤 2=漂洗 3=脱水)
    DEV_CURTAIN,        // 窗帘 (value = 开合百分比 0-100)
    DEV_AIR_PURIFIER,   // 空气净化器 (value = 风速 0-3)
    DEV_ARC,            // 温度调节旋钮 (value = 设定温度)
} DeviceType;

// ── Device struct ────────────────────────────────────────
typedef struct Device {
    int         id;
    char        name[32];
    DeviceType  type;
    int         state;          // 0=off, 1=on
    int         value;          // 含义随类型变化
    int         pid;            // 子进程 PID
    int         cmd_fd;         // 写端 fd → 发送命令给子进程
    int         resp_fd;        // 读端 fd ← 子进程返回状态
    struct Device *next;
} Device;

// ── Device list ──────────────────────────────────────────
typedef struct {
    Device *head;
    int     count;
    int     next_id;
} DeviceManager;

// ── Public API ───────────────────────────────────────────
void dev_mgr_init(DeviceManager *mgr);
void dev_mgr_shutdown(DeviceManager *mgr);
void dev_mgr_poll(DeviceManager *mgr);  // 非阻塞轮询设备状态更新

Device *dev_mgr_add(DeviceManager *mgr, const char *name, DeviceType type);
void    dev_mgr_remove(DeviceManager *mgr, int id);
Device *dev_mgr_find_by_id(DeviceManager *mgr, int id);
Device *dev_mgr_find_by_type(DeviceManager *mgr, DeviceType type);

// 发送命令到设备进程
void dev_send_command(Device *dev, const char *cmd);

// 遍历链表
#define dev_foreach(mgr, dev) \
    for (Device *dev = (mgr)->head; dev; dev = dev->next)

#ifdef __cplusplus
}
#endif

#endif
