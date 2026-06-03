// audio_output.c — minimp3 解码 + OSS 播放
#include "audio_output.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/soundcard.h>
#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"

static volatile int g_audio_playing = 0;
static volatile int g_audio_paused  = 0;
static volatile int g_audio_time_ms = 0;
static volatile int g_audio_duration_ms = 0;
static volatile int g_audio_volume = 95;
static volatile int g_thread_alive = 0;
static volatile int g_seek_ms  = -1;
static volatile long g_data_start = 0;
static volatile long g_file_size  = 0;
static pthread_t g_audio_thread;

void audio_init(void) {}

static void *play_thread(void *arg)
{
    const char *path = (const char *)arg;
    int fd = open(path, O_RDONLY);
    if (fd < 0) { free(arg); g_audio_playing = 0; return NULL; }

    // 跳过 ID3v2 标签头
    // ID3v2 头部：前3字节="ID3"，字节6-9用 sync-safe 整数编码标签大小
    // sync-safe：每字节最高位为0，4字节拼成28位有效值，避免出现伪同步码
    char id3[10];
    read(fd, id3, 10);
    if (memcmp(id3, "ID3", 3) == 0) {
        int s = ((id3[6] & 0x7f) << 21) | ((id3[7] & 0x7f) << 14)
              | ((id3[8] & 0x7f) << 7)  | (id3[9] & 0x7f);
        lseek(fd, s + 10, SEEK_SET);
    } else {
        lseek(fd, 0, SEEK_SET);
    }

    // 读取完整文件大小供 seek 和时长估算用
    off_t fsize = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    // 重新跳过 ID3 标签，定位到真正的 MPEG 数据起始位置
    read(fd, id3, 10);
    if (memcmp(id3, "ID3", 3) == 0) {
        int s = ((id3[6] & 0x7f) << 21) | ((id3[7] & 0x7f) << 14)
              | ((id3[8] & 0x7f) << 7)  | (id3[9] & 0x7f);
        lseek(fd, s + 10, SEEK_SET);
    } else lseek(fd, 0, SEEK_SET);
    off_t data_start = lseek(fd, 0, SEEK_CUR);  // 记录 MPEG 数据起始偏移
    g_data_start = data_start;
    g_file_size  = fsize;
    // 时长在首帧解码获得真实采样率后计算

    mp3dec_t mp3;
    mp3dec_init(&mp3);
    mp3dec_frame_info_t info;
    short pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    uint8_t buf[65536];
    int buf_len = read(fd, buf, sizeof(buf));
    int buf_pos = 0, rate = 44100;
    int dsp = -1, dsp_opened = 0, total_samples = 0;

    if (buf_len <= 0) { fprintf(stderr, "[音频] 文件为空\n"); fflush(stderr); close(fd); free(arg); g_audio_playing = 0; return NULL; }

    fprintf(stderr, "[音频] 解码中...\n"); fflush(stderr);
    while (g_audio_playing) {
        // —— Seek 处理：按字节比例估算目标位置，再对齐到最近的 MPEG 帧同步字 ——
        // MPEG Audio 帧同步字：0xFFE0（11个连续1），在文件中搜索 0xFF 且下一字节高3位全1
        if (g_seek_ms >= 0 && g_audio_duration_ms > 0) {
            long byte_pos = g_file_size * (long long)g_seek_ms / g_audio_duration_ms;
            lseek(fd, g_data_start + byte_pos, SEEK_SET);
            char t[4096]; int n = read(fd, t, sizeof(t)), s = 0;
            while (s < n-1 && !(t[s]==0xff && (t[s+1]&0xe0)==0xe0)) s++;
            lseek(fd, g_data_start + byte_pos + s, SEEK_SET);
            buf_len = 0; buf_pos = 0;
            total_samples = (long long)g_seek_ms * rate / 1000;
            g_audio_time_ms = g_seek_ms;
            g_seek_ms = -1;
        }

        // 暂停时忙等待（100ms 间隔避免 CPU 空转）
        while (g_audio_paused && g_audio_playing) usleep(100000);

        // 缓冲区剩余不足 4KB 时补充数据，保证解码器有足够帧数据
        if (buf_len - buf_pos < 4096) {
            memmove(buf, buf + buf_pos, buf_len - buf_pos);
            buf_len -= buf_pos; buf_pos = 0;
            int n = read(fd, buf + buf_len, sizeof(buf) - buf_len);
            if (n <= 0) break;
            buf_len += n;
        }

        // 逐帧解码 MP3 → PCM
        int samples = mp3dec_decode_frame(&mp3, buf + buf_pos, buf_len - buf_pos, pcm, &info);
        if (samples <= 0) { buf_pos++; continue; }  // 未找到帧头则前进1字节重试
        buf_pos += info.frame_bytes;

        // 首帧解码成功后打开 OSS 声卡设备，根据实际音频参数配置
        if (!dsp_opened) {
            rate = info.hz;
            dsp = open("/dev/dsp", O_WRONLY);
            if (dsp < 0) dsp = open("/dev/dsp1", O_WRONLY);
            if (dsp < 0) { close(fd); free(arg); g_audio_playing = 0; return NULL; }
            int f = AFMT_S16_LE, c = info.channels, r = info.hz;
            ioctl(dsp, SNDCTL_DSP_SETFMT, &f);
            ioctl(dsp, SNDCTL_DSP_CHANNELS, &c);
            ioctl(dsp, SNDCTL_DSP_SPEED, &r);
            dsp_opened = 1;
            fprintf(stderr, "[音频] 声卡已开 采样率=%d 声道=%d\n", info.hz, info.channels); fflush(stderr);
        }

        total_samples += samples;
        g_audio_time_ms = (int)((long long)total_samples * 1000 / rate);

        // 音量
        for (int i = 0; i < samples * info.channels; i++)
            pcm[i] = (short)((int)pcm[i] * g_audio_volume / 100);
        write(dsp, pcm, samples * info.channels * 2);
    }

    if (dsp_opened) close(dsp);
    close(fd);
    free(arg);
    g_audio_playing = 0;
    g_thread_alive   = 0;
    return NULL;
}

void video_stop(void);  // 音乐播放时停视频

void audio_play(const char *filepath)
{
    fprintf(stderr, "[音频] 播放: %s\n", filepath); fflush(stderr);
    video_stop();
    audio_stop();
    char *copy = strdup(filepath);
    g_audio_playing = 1;
    g_audio_paused  = 0;
    g_audio_time_ms = 0;
    g_thread_alive   = 1;
    pthread_create(&g_audio_thread, NULL, play_thread, copy);
    pthread_detach(g_audio_thread);
}

void audio_stop(void)
{
    fprintf(stderr, "[音频] 停止\n"); fflush(stderr);
    g_audio_playing = 0;
    g_audio_paused  = 0;
    for (int i = 0; i < 20 && g_thread_alive; i++)
        usleep(50000);  // 等最多 1 秒
}

void audio_pause(void)  { g_audio_paused = 1; }
void audio_resume(void) { g_audio_paused = 0; }
int  audio_is_playing(void) { return g_audio_playing && !g_audio_paused; }
int  audio_is_paused(void)  { return g_audio_paused; }
int  audio_get_time_ms(void) { return g_audio_time_ms; }
int  audio_get_duration_ms(void) { return g_audio_duration_ms; }
void audio_seek(int ms)          { if (ms >= 0) g_seek_ms = ms; }
void audio_set_duration(int ms)  { if (ms > 0) g_audio_duration_ms = ms; }
void audio_set_volume(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    g_audio_volume = (pct <= 1) ? 0 : pct;
    fprintf(stderr, "[音频] 音量=%d%% soft=%d\n", pct, g_audio_volume); fflush(stderr);

    // 硬件混音器
    int vol = (g_audio_volume << 8) | g_audio_volume;
    for (int d = 0; d < 2; d++) {
        char dev[16];
        snprintf(dev, sizeof(dev), "/dev/mixer%s", d ? "1" : "");
        int fd = open(dev, O_RDWR);
        if (fd >= 0) {
            ioctl(fd, MIXER_WRITE(SOUND_MIXER_VOLUME), &vol);
            close(fd);
        }
    }
}
