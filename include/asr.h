#ifndef ASR_H
#define ASR_H

// 启动语音识别, callback 在子线程中调用, 不要直接操作 LVGL
void asr_start_listening(void (*callback)(const char *text));

// 停止识别
void asr_stop_and_cleanup(void);

// 返回 1 表示正在录音中
int asr_is_listening(void);

#endif
