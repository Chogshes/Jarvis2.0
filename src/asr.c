// asr.c — OSS 录音 + WebSocket 发送到 Vosk
#include "asr.h"
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
// ======================== 配置 ========================
#define VOSK_SERVER_IP    "192.168.64.94"
#define VOSK_SERVER_PORT  2700
#define AUDIO_SAMPLE_RATE 16000
#define AUDIO_CHANNELS    1
#define AUDIO_SAMPLES     1024
#define AUDIO_BUF_SIZE    (AUDIO_SAMPLES * 2)
#define WS_TIMEOUT_SEC    8
#define MAX_RESULT_LEN    2048

// ==================== WebSocket 帧操作 ====================

static int ws_send_binary(int sock, const uint8_t *data, int len) {
    uint8_t frame[4096 + 14];
    int header_len;

    frame[0] = 0x82;  // FIN=1, opcode=0x2 (binary)

    if (len <= 125) {
        frame[1] = 0x80 | (uint8_t)len;
        header_len = 2;
    } else if (len <= 65535) {
        frame[1] = 0x80 | 126;
        frame[2] = (len >> 8) & 0xFF;
        frame[3] = len & 0xFF;
        header_len = 4;
    } else {
        frame[1] = 0x80 | 127;
        memset(frame + 2, 0, 4);
        frame[6] = (len >> 24) & 0xFF;
        frame[7] = (len >> 16) & 0xFF;
        frame[8] = (len >> 8) & 0xFF;
        frame[9] = len & 0xFF;
        header_len = 10;
    }

    uint8_t mask[4] = {0x12, 0x34, 0x56, 0x78};
    memcpy(frame + header_len, mask, 4);
    header_len += 4;

    for (int i = 0; i < len; i++)
        frame[header_len + i] = data[i] ^ mask[i % 4];

    return send(sock, frame, header_len + len, 0);
}

static int ws_send_text(int sock, const char *text) {
    uint8_t frame[1024];
    int len = strlen(text);
    int header_len;

    frame[0] = 0x81;
    if (len <= 125) {
        frame[1] = 0x80 | (uint8_t)len;
        header_len = 2;
    } else {
        frame[1] = 0x80 | 126;
        frame[2] = (len >> 8) & 0xFF;
        frame[3] = len & 0xFF;
        header_len = 4;
    }

    uint8_t mask[4] = {0x12, 0x34, 0x56, 0x78};
    memcpy(frame + header_len, mask, 4);
    header_len += 4;

    for (int i = 0; i < len; i++)
        frame[header_len + i] = text[i] ^ mask[i % 4];

    return send(sock, frame, header_len + len, 0);
}

static int ws_recv(int sock, char *buf, int buf_size, int timeout_sec) {
    fd_set fds;
    struct timeval tv;
    FD_ZERO(&fds);
    FD_SET(sock, &fds);
    tv.tv_sec = timeout_sec;
    tv.tv_usec = 0;

    if (select(sock + 1, &fds, NULL, NULL, &tv) <= 0) return -1;

    uint8_t header[2];
    int n = recv(sock, header, 2, 0);
    if (n != 2) return -1;

    int opcode = header[0] & 0x0F;
    int masked = (header[1] >> 7) & 1;
    uint64_t payload_len = header[1] & 0x7F;

    if (payload_len == 126) {
        uint8_t ext[2]; recv(sock, ext, 2, 0);
        payload_len = (ext[0] << 8) | ext[1];
    } else if (payload_len == 127) {
        uint8_t ext[8]; recv(sock, ext, 8, 0);
        payload_len = 0;
        for (int i = 0; i < 8; i++) payload_len = (payload_len << 8) | ext[i];
    }

    uint8_t mask_key[4] = {0};
    if (masked) recv(sock, mask_key, 4, 0);

    if (payload_len >= (uint64_t)buf_size) payload_len = buf_size - 1;
    n = recv(sock, buf, (int)payload_len, 0);
    if (n <= 0) return -1;

    if (masked)
        for (int i = 0; i < n; i++) buf[i] ^= mask_key[i % 4];
    buf[n] = '\0';

    if (opcode == 0x8) return -1;
    if (opcode == 0x9) {
        uint8_t pong[6] = {0x8A, 0x80, 0x00, 0x00, 0x00, 0x00};
        send(sock, pong, 6, 0);
        return 0;
    }
    return n;
}

// ==================== WebSocket 连接 ====================

static int ws_connect_to_vosk(void) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    struct hostent *host = gethostbyname(VOSK_SERVER_IP);
    if (!host) { close(sock); return -1; }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(VOSK_SERVER_PORT);
    memcpy(&addr.sin_addr.s_addr, host->h_addr, host->h_length);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock); return -1;
    }

    char request[512];
    snprintf(request, sizeof(request),
        "GET / HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: x3JJHMbDL1EzLkh9GBhXDw==\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n",
        VOSK_SERVER_IP, VOSK_SERVER_PORT);

    send(sock, request, strlen(request), 0);

    char response[1024];
    int n = recv(sock, response, sizeof(response) - 1, 0);
    if (n <= 0 || strstr(response, "101") == NULL) {
        close(sock); return -1;
    }
    return sock;
}

// ==================== JSON 解析 ====================

static int parse_vosk_result(const char *data, char *result, int max_len) {
    const char *key = strstr(data, "\"text\"");
    if (!key) return 0;
    key += 6;
    while (*key == ' ' || *key == ':' || *key == '"') key++;
    const char *end = strchr(key, '"');
    if (!end || end <= key) return 0;
    int len = end - key;
    if (len >= max_len) len = max_len - 1;
    memcpy(result, key, len);
    result[len] = '\0';
    return len;
}

// ==================== 全局状态 ====================
static volatile int g_listening = 0;
static volatile int g_stop_flag = 0;
static int g_ws_sock = -1;
static pthread_t g_record_thread;
static void (*g_callback)(const char *) = NULL;
static char g_final_result[MAX_RESULT_LEN];

// ==================== 录音线程 ====================

static void *record_thread(void *arg)
{
    (void)arg;

    // 1. 连接 Vosk WebSocket
    fprintf(stderr, "[ASR] Connecting to Vosk %s:%d ...\n", VOSK_SERVER_IP, VOSK_SERVER_PORT);
    g_ws_sock = ws_connect_to_vosk();
    if (g_ws_sock < 0) {
        fprintf(stderr, "[ASR] FAILED to connect to Vosk\n");
        g_listening = 0;
        return NULL;
    }
    fprintf(stderr, "[ASR] WebSocket connected (fd=%d)\n", g_ws_sock);

    // OSS 录音 — 零依赖库，纯内核API
    // 先尝试设置 mixer 音量
    int mixer_fd = open("/dev/mixer", O_RDWR);
    if (mixer_fd >= 0) {
        int vol = (95 << 8) | 95;  // 左右声道 95%
        int src = SOUND_MIXER_MIC;  // 选择麦克风输入源
        ioctl(mixer_fd, MIXER_WRITE(SOUND_MIXER_MIC),     &vol);
        ioctl(mixer_fd, MIXER_WRITE(SOUND_MIXER_VOLUME),  &vol);
        ioctl(mixer_fd, MIXER_WRITE(SOUND_MIXER_IGAIN),   &vol);
        ioctl(mixer_fd, MIXER_WRITE(SOUND_MIXER_LINE),    &vol);
        ioctl(mixer_fd, MIXER_WRITE(SOUND_MIXER_RECLEV),  &vol);
        ioctl(mixer_fd, MIXER_WRITE(SOUND_MIXER_RECSRC),  &src);
        close(mixer_fd);
        fprintf(stderr, "[ASR] Mixer set to 95%%\n");
    }

    int dsp_fd = open("/dev/dsp", O_RDONLY);
    if (dsp_fd < 0) {
        fprintf(stderr, "[ASR] OSS /dev/dsp open failed\n");
        close(g_ws_sock); g_ws_sock = -1;
        g_listening = 0;
        return NULL;
    }
    int fmt = AFMT_U16_LE;
    if (ioctl(dsp_fd, SNDCTL_DSP_SETFMT, &fmt) < 0)
        fprintf(stderr, "[ASR] SETFMT failed\n");
    int ch  = 1;
    if (ioctl(dsp_fd, SNDCTL_DSP_CHANNELS, &ch) < 0)
        fprintf(stderr, "[ASR] CHANNELS failed\n");
    int rate = AUDIO_SAMPLE_RATE;
    if (ioctl(dsp_fd, SNDCTL_DSP_SPEED, &rate) < 0)
        fprintf(stderr, "[ASR] SPEED failed\n");
    fprintf(stderr, "[ASR] fmt=%d ch=%d rate=%d recording...\n", fmt, ch, rate);

    short buf[AUDIO_SAMPLES];
    int frames = 0;
    while (!g_stop_flag) {
        // 读音频
        int n = read(dsp_fd, buf, sizeof(buf));
        if (n > 0) {
            // U16 → S16 + 2倍增益
            for (int i = 0; i < n / 2; i++) {
                int s = ((buf[i] & 0xffff) - 0x8000) * 2;
                if (s > 32767) s = 32767;
                if (s < -32768) s = -32768;
                buf[i] = (short)s;
            }
            ws_send_binary(g_ws_sock, (uint8_t *)buf, n);
            frames++;
            // 必须读 Vosk 返回的 partial，否则服务器 send buffer 会满
            char tmp[256];
            int r = ws_recv(g_ws_sock, tmp, sizeof(tmp), 0);
            if (r > 0) {
                char text[1024];
                if (parse_vosk_result(tmp, text, sizeof(text)) > 0)
                    strncpy(g_final_result, text, MAX_RESULT_LEN - 1);
                if (frames <= 3 || strstr(tmp, "\"result\""))
                    fprintf(stderr, "[ASR] partial[%d]: %s\n", frames, tmp);
            }
        }
    }
    fprintf(stderr, "[ASR] Total frames sent: %d\n", frames);
    close(dsp_fd);

    fprintf(stderr, "[ASR] Audio stopped\n");

    // 5. 发送 EOF
    ws_send_text(g_ws_sock, "{\"eof\" : 1}");
    fprintf(stderr, "[ASR] EOF sent, waiting for Vosk result...\n");

    // 6. 接收识别结果
    time_t start = time(NULL);
    g_final_result[0] = '\0';
    int recv_count = 0;

    while (time(NULL) - start < WS_TIMEOUT_SEC) {
        char payload[2048];
        int n = ws_recv(g_ws_sock, payload, sizeof(payload), 1);
        if (n > 0) {
            recv_count++;
            fprintf(stderr, "[ASR] Vosk msg[%d]: %s\n", recv_count, payload);
            char text[1024];
            if (parse_vosk_result(payload, text, sizeof(text)) > 0)
                strncpy(g_final_result, text, MAX_RESULT_LEN - 1);
            if (strstr(payload, "\"result\"")) break;
        } else if (n < 0) {
            break;
        }
    }

    // 7. 清理
    close(g_ws_sock);
    g_ws_sock = -1;
    g_listening = 0;
    g_stop_flag = 0;

    // 8. 回调
    if (g_final_result[0] != '\0' && g_callback) {
        fprintf(stderr, "[ASR] Final result: '%s'\n", g_final_result);
        g_callback(g_final_result);
    } else {
        fprintf(stderr, "[ASR] No final result\n");
    }
    fprintf(stderr, "[ASR] Thread exiting\n");
    return NULL;
}

// ==================== 对外接口 ====================

void asr_start_listening(void (*callback)(const char *text)) {
    if (g_listening) return;
    g_callback = callback;
    g_listening = 1;
    g_stop_flag = 0;
    g_ws_sock = -1;
    memset(g_final_result, 0, sizeof(g_final_result));
    pthread_create(&g_record_thread, NULL, record_thread, NULL);
    pthread_detach(g_record_thread);
}

void asr_stop_and_cleanup(void) {
    if (!g_listening) return;
    g_stop_flag = 1;
    for (int i = 0; i < 100 && g_listening; i++)
        usleep(100000);
}

int asr_is_listening(void) {
    return g_listening;
}
