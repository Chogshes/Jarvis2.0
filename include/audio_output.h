// audio_output.h — minimp3 播放控制
#ifndef AUDIO_OUTPUT_H
#define AUDIO_OUTPUT_H

void audio_init(void);
void audio_play(const char *filepath);
void audio_stop(void);
void audio_pause(void);
void audio_resume(void);
int  audio_is_playing(void);
int  audio_is_paused(void);
int  audio_get_time_ms(void);       // 当前播放进度（毫秒）
int  audio_get_duration_ms(void);   // 估算总时长（毫秒）
void audio_set_volume(int pct);     // 设置音量 0-100
void audio_seek(int ms);            // 跳转到指定毫秒位置
void audio_set_duration(int ms);    // 设置总时长（毫秒）

#endif
