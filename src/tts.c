// tts.c — HTTP TTS API + OSS 播放
#include "tts.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/soundcard.h>

#define TTS_SERVER_IP    "192.168.64.94"
#define TTS_SERVER_PORT  9881
#define TTS_API_PATH     "/v1/audio/speech"
#define TTS_VOICE        "alloy"
#define TTS_MAX_BUF      (2 * 1024 * 1024)

#pragma pack(push, 1)
typedef struct {
    char     riff[4];
    uint32_t file_size;
    char     wave[4];
    char     fmt[4];
    uint32_t fmt_size;
    uint16_t audio_format;
    uint16_t channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
    char     data_marker[4];
    uint32_t data_size;
} WavHeader;
#pragma pack(pop)

static void json_escape(const char *src, char *dst, int max) {
    const char *s = src;
    char *d = dst;
    while (*s && (d - dst) < max - 10) {
        if (*s == '"')       { *d++ = '\\'; *d++ = '"'; }
        else if (*s == '\\') { *d++ = '\\'; *d++ = '\\'; }
        else if (*s == '\n') { *d++ = '\\'; *d++ = 'n'; }
        else if (*s == '\r') { *d++ = '\\'; *d++ = 'r'; }
        else *d++ = *s;
        s++;
    }
    *d = '\0';
}

static uint8_t *http_tts_request(const char *text, uint32_t *out_len) {
    char escaped[512], body[1024];
    json_escape(text, escaped, sizeof(escaped));
    int body_len = snprintf(body, sizeof(body),
        "{\"model\":\"tts-1\",\"input\":\"%s\","
        "\"voice\":\"%s\",\"response_format\":\"wav\"}",
        escaped, TTS_VOICE);

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return NULL;

    struct hostent *host = gethostbyname(TTS_SERVER_IP);
    if (!host) { close(sock); return NULL; }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(TTS_SERVER_PORT);
    memcpy(&addr.sin_addr.s_addr, host->h_addr, host->h_length);

    struct timeval tv = {15, 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock); return NULL;
    }

    char header[512];
    int hdr_len = snprintf(header, sizeof(header),
        "POST %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n\r\n",
        TTS_API_PATH, TTS_SERVER_IP, TTS_SERVER_PORT, body_len);

    if (send(sock, header, hdr_len, 0) != hdr_len) { close(sock); return NULL; }
    if (send(sock, body, body_len, 0) != body_len) { close(sock); return NULL; }

    uint8_t *buf = malloc(TTS_MAX_BUF);
    if (!buf) { close(sock); return NULL; }
    int total = 0, n;
    while ((n = recv(sock, buf + total, TTS_MAX_BUF - total - 1, 0)) > 0)
        total += n;
    close(sock);
    fprintf(stderr, "[TTS] HTTP total=%d\n", total);
    if (total == 0) { free(buf); return NULL; }
    // 打印 HTTP 响应头
    buf[total] = '\0';
    char *hdr_end = strstr((char *)buf, "\r\n\r\n");
    if (hdr_end) {
        int hdr_len = hdr_end - (char *)buf;
        fprintf(stderr, "[TTS] headers (%d bytes): %.*s\n", hdr_len, hdr_len < 500 ? hdr_len : 500, buf);
    }

    // HTTP 头/体由空行 \r\n\r\n 分隔，定位到 WAV 数据起始位置
    int body_start = -1;
    for (int i = 0; i < total - 3; i++) {
        if (buf[i] == '\r' && buf[i+1] == '\n' && buf[i+2] == '\r' && buf[i+3] == '\n') {
            body_start = i + 4; break;
        }
    }
    if (body_start < 0) { free(buf); return NULL; }

    uint32_t data_len = total - body_start;
    uint8_t *wav = malloc(data_len);
    if (!wav) { free(buf); return NULL; }
    memcpy(wav, buf + body_start, data_len);
    free(buf);
    *out_len = data_len;
    return wav;
}

// ========= 音频播放 (OSS) =========
#define TTS_GAIN 200  // 200% 音量增益

static int oss_play_raw(const void *pcm_data, uint32_t pcm_len) {
    if (!pcm_data || pcm_len < 100) return -1;
    int dsp_fd = open("/dev/dsp1", O_WRONLY);
    if (dsp_fd < 0) dsp_fd = open("/dev/dsp", O_WRONLY);
    if (dsp_fd < 0) { fprintf(stderr, "[TTS] dsp open failed\n"); return -1; }

    int fmt  = AFMT_U16_LE, ch = 1, rate = 8000;
    ioctl(dsp_fd, SNDCTL_DSP_SETFMT,   &fmt);
    ioctl(dsp_fd, SNDCTL_DSP_CHANNELS, &ch);
    ioctl(dsp_fd, SNDCTL_DSP_SPEED,    &rate);

    // S16 PCM → 增益 → U16 硬件
    int nsamp = (int)pcm_len / 2;
    short *buf = malloc(pcm_len);
    memcpy(buf, pcm_data, pcm_len);
    for (int i = 0; i < nsamp; i++) {
        int s = (int)buf[i] * TTS_GAIN / 100;
        if (s > 32767)  s = 32767;
        if (s < -32768) s = -32768;
        buf[i] = (short)s ^ 0x8000;
    }

    int wrote = write(dsp_fd, buf, pcm_len);
    fprintf(stderr, "[TTS] fmt=%d ch=%d rate=%d len=%u wrote=%d\n", fmt, ch, rate, pcm_len, wrote);
    free(buf);
    close(dsp_fd);
    return 0;
}
#define platform_play_wav oss_play_raw

// ========= 播放线程 =========
static void *play_thread_func(void *arg) {
    char *text = (char *)arg;

    uint32_t wav_len;
    uint8_t *wav = http_tts_request(text, &wav_len);
    free(text);
    if (!wav) return NULL;

    platform_play_wav(wav, wav_len);
    free(wav);
    return NULL;
}

// ========= 对外接口 =========
void tts_speak(const char *text) {
    if (!text || !*text) return;
    char *copy = strdup(text);
    if (!copy) return;
    pthread_t tid;
    pthread_create(&tid, NULL, play_thread_func, copy);
    pthread_detach(tid);
}

void tts_speak_sync(const char *text) {
    if (!text || !*text) return;
    char *copy = strdup(text);
    if (!copy) return;
    pthread_t tid;
    pthread_create(&tid, NULL, play_thread_func, copy);
    pthread_join(tid, NULL);
}
