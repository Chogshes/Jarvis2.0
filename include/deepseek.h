#ifndef DEEPSEEK_H
#define DEEPSEEK_H

typedef struct {
    char intent_type[16];
    char device_name[32];
    char action[16];
    int  param_value;
} Intent;

// 在新线程中调用 DeepSeek, 解析完成后回调 on_result
// on_result 在子线程中调用, 不要直接操作 LVGL
void deepseek_send_request_async(const char *user_text,
                                  void (*on_result)(const Intent *, const char *reply));
#endif
