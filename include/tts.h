// tts.h — 局域网 TTS API 客户端，不依赖外部命令
#ifndef TTS_H
#define TTS_H

// 在新线程中请求 TTS 并播放，不会阻塞 UI
void tts_speak(const char *text);

// 阻塞版本，播放完才返回
void tts_speak_sync(const char *text);

#endif
