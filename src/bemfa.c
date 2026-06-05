#include "bemfa.h"
#include "lvgl/lvgl.h"
#include "ui.h"
#include "ui_events.h"
#include "music.h"
#include "video.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

#define BEMFA_HOST "bemfa.com"
#define BEMFA_PORT "8344"
#define BEMFA_RECONNECT_SEC 5
#define BEMFA_HEARTBEAT_SEC 50
#define BEMFA_DEVICE_MUSIC (-1001)
#define BEMFA_DEVICE_VIDEO (-1002)

#ifndef BEMFA_UID_DEFAULT
#define BEMFA_UID_DEFAULT ""
#endif

typedef struct {
    const char *topic;
    int device;
} BemfaDeviceTopic;

typedef struct {
    char topic[96];
    char msg[128];
} BemfaCommand;

static const BemfaDeviceTopic g_topics[] = {
    {"thermostat010", HOME_DEVICE_THERMOMETER},
    {"ac005", HOME_DEVICE_AC},
    {"airpurifier013", HOME_DEVICE_AIR_PURIFIER},
    {"masterlight002", HOME_DEVICE_MASTER_LIGHT},
    {"livinglight002", HOME_DEVICE_LIVING_LIGHT},
    {"kitchenlight002", HOME_DEVICE_KITCHEN_LIGHT},
    {"refrigerator001", HOME_DEVICE_REFRIGERATOR},
    {"washer001", HOME_DEVICE_WASHER},
    {"curtain009", HOME_DEVICE_CURTAIN},
    {"music012", BEMFA_DEVICE_MUSIC},
    {"video012", BEMFA_DEVICE_VIDEO},
};

static pthread_t g_thread;
static pthread_mutex_t g_sock_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_running = 0;
static int g_sock = -1;
static char g_uid[64] = BEMFA_UID_DEFAULT;

static int clamp_int(int value, int min_value, int max_value)
{
    if (value < min_value) return min_value;
    if (value > max_value) return max_value;
    return value;
}

static const BemfaDeviceTopic *find_topic(const char *topic)
{
    if (!topic) return NULL;
    for (size_t i = 0; i < sizeof(g_topics) / sizeof(g_topics[0]); i++) {
        size_t topic_len = strlen(g_topics[i].topic);
        if (strcmp(topic, g_topics[i].topic) == 0)
            return &g_topics[i];
        if (strncmp(topic, g_topics[i].topic, topic_len) == 0 && topic[topic_len] == '/')
            return &g_topics[i];
    }
    return NULL;
}

static int is_light_device(int device)
{
    return device == HOME_DEVICE_MASTER_LIGHT ||
           device == HOME_DEVICE_LIVING_LIGHT ||
           device == HOME_DEVICE_KITCHEN_LIGHT;
}

static int is_media_device(int device)
{
    return device == BEMFA_DEVICE_MUSIC || device == BEMFA_DEVICE_VIDEO;
}

static int socket_send_all(int sock, const char *data)
{
    size_t len = strlen(data);
    while (len > 0) {
        ssize_t n = send(sock, data, len, 0);
        if (n <= 0) return -1;
        data += n;
        len -= (size_t)n;
    }
    return 0;
}

static int connect_bemfa(void)
{
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    struct addrinfo *p;
    int sock = -1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(BEMFA_HOST, BEMFA_PORT, &hints, &res) != 0)
        return -1;

    for (p = res; p; p = p->ai_next) {
        sock = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sock < 0) continue;
        if (connect(sock, p->ai_addr, p->ai_addrlen) == 0)
            break;
        close(sock);
        sock = -1;
    }

    freeaddrinfo(res);
    return sock;
}

static int subscribe_topics(int sock)
{
    char topic_buf[256];
    size_t i = 0;

    while (i < sizeof(g_topics) / sizeof(g_topics[0])) {
        int count = 0;
        topic_buf[0] = '\0';

        while (i < sizeof(g_topics) / sizeof(g_topics[0]) && count < 8) {
            if (count > 0)
                strncat(topic_buf, ",", sizeof(topic_buf) - strlen(topic_buf) - 1);
            strncat(topic_buf, g_topics[i].topic, sizeof(topic_buf) - strlen(topic_buf) - 1);
            i++;
            count++;
        }

        char cmd[384];
        snprintf(cmd, sizeof(cmd), "cmd=1&uid=%s&topic=%s\r\n", g_uid, topic_buf);
        if (socket_send_all(sock, cmd) < 0)
            return -1;
    }

    return 0;
}

static void set_active_socket(int sock)
{
    pthread_mutex_lock(&g_sock_mutex);
    g_sock = sock;
    pthread_mutex_unlock(&g_sock_mutex);
}

void bemfa_publish_state(const char *topic, const char *msg)
{
    char cmd[256];

    if (!topic || !msg || !g_uid[0]) return;

    snprintf(cmd, sizeof(cmd), "cmd=2&uid=%s&topic=%s/up&msg=%s\r\n", g_uid, topic, msg);

    pthread_mutex_lock(&g_sock_mutex);
    if (g_sock >= 0)
        socket_send_all(g_sock, cmd);
    pthread_mutex_unlock(&g_sock_mutex);
}

static int parse_number(const char *msg, int *value)
{
    if (!msg || !value) return 0;
    while (*msg) {
        if (*msg >= '0' && *msg <= '9') {
            *value = atoi(msg);
            return 1;
        }
        msg++;
    }
    return 0;
}

static int is_off_msg(const char *msg)
{
    return msg &&
           (strcasecmp(msg, "off") == 0 ||
            strcasecmp(msg, "close") == 0 ||
            strcmp(msg, "0") == 0);
}

static int is_on_msg(const char *msg)
{
    return msg &&
           (strcasecmp(msg, "on") == 0 ||
            strcasecmp(msg, "open") == 0 ||
            strcmp(msg, "1") == 0);
}

static void handle_device_command(BemfaCommand *cmd)
{
    const char *msg = cmd->msg;
    const BemfaDeviceTopic *entry = find_topic(cmd->topic);
    const char *topic;
    int value = 0;
    char state_msg[32];

    if (!entry || !msg) return;
    topic = entry->topic;

    if (entry->device == BEMFA_DEVICE_MUSIC) {
        if (is_off_msg(msg)) {
            music_stop();
            bemfa_publish_state(topic, "off");
        } else if (is_on_msg(msg) || strcasecmp(msg, "play") == 0) {
            int index = music_get_current_index();
            if (index < 0) index = 0;
            music_play(index);
            bemfa_publish_state(topic, "on");
        }
        return;
    }

    if (entry->device == BEMFA_DEVICE_VIDEO) {
        if (is_off_msg(msg)) {
            video_stop();
            bemfa_publish_state(topic, "off");
        } else if (is_on_msg(msg) || strcasecmp(msg, "play") == 0) {
            video_play_current();
            bemfa_publish_state(topic, "on");
        }
        return;
    }

    if (entry->device == HOME_DEVICE_AC) {
        if (is_off_msg(msg)) {
            home_set_device_power(entry->device, 0);
            bemfa_publish_state(topic, "off");
            return;
        }
        if (parse_number(msg, &value)) {
            value = clamp_int(value, 16, 35);
            home_set_ac_temperature(value);
            snprintf(state_msg, sizeof(state_msg), "%d", value);
            bemfa_publish_state(topic, state_msg);
            return;
        }
        home_set_ac_temperature(25);
        bemfa_publish_state(topic, "25");
        return;
    }

    if (is_light_device(entry->device)) {
        if (is_off_msg(msg)) {
            home_set_light_brightness(entry->device, 0);
            bemfa_publish_state(topic, "off");
            return;
        }
        if (parse_number(msg, &value)) {
            value = clamp_int(value, 0, 100);
            home_set_light_brightness(entry->device, value);
            snprintf(state_msg, sizeof(state_msg), "%d", value);
            bemfa_publish_state(topic, state_msg);
            return;
        }
        home_set_light_brightness(entry->device, 20);
        bemfa_publish_state(topic, "20");
        return;
    }

    if (is_off_msg(msg)) {
        home_set_device_power(entry->device, 0);
        bemfa_publish_state(topic, "off");
    } else if (is_on_msg(msg) || parse_number(msg, &value)) {
        home_set_device_power(entry->device, 1);
        bemfa_publish_state(topic, "on");
    }
}

static void handle_device_async(void *user_data)
{
    BemfaCommand *cmd = (BemfaCommand *)user_data;
    const BemfaDeviceTopic *entry;
    if (!cmd) return;
    entry = find_topic(cmd->topic);
    if (entry && entry->device == BEMFA_DEVICE_MUSIC && lv_scr_act() != ui_musicScreen)
        _ui_screen_change(&ui_musicScreen, LV_SCR_LOAD_ANIM_FADE_ON, 300, 0, ui_musicScreen_screen_init);
    else if (entry && entry->device == BEMFA_DEVICE_VIDEO && lv_scr_act() != ui_videoScreen)
        _ui_screen_change(&ui_videoScreen, LV_SCR_LOAD_ANIM_FADE_ON, 300, 0, ui_videoScreen_screen_init);
    else if (entry && !is_media_device(entry->device) && lv_scr_act() != ui_houseScreen)
        _ui_screen_change(&ui_houseScreen, LV_SCR_LOAD_ANIM_FADE_ON, 300, 0, ui_houseScreen_screen_init);
    handle_device_command(cmd);
    free(cmd);
}

static void queue_device_message(const char *topic, const char *msg)
{
    BemfaCommand *cmd;

    if (!topic || !msg) return;
    cmd = calloc(1, sizeof(*cmd));
    if (!cmd) return;

    strncpy(cmd->topic, topic, sizeof(cmd->topic) - 1);
    strncpy(cmd->msg, msg, sizeof(cmd->msg) - 1);

    if (lv_async_call(handle_device_async, cmd) != LV_RES_OK)
        free(cmd);
}

static int get_query_value(const char *line, const char *key, char *out, size_t out_size)
{
    const char *p = line;
    size_t key_len = strlen(key);

    if (!line || !key || !out || out_size == 0) return 0;

    while ((p = strstr(p, key)) != NULL) {
        const char *start;
        const char *end;
        if (p != line && *(p - 1) != '&') {
            p += key_len;
            continue;
        }
        if (p[key_len] != '=') {
            p += key_len;
            continue;
        }
        start = p + key_len + 1;
        end = strchr(start, '&');
        if (!end) end = start + strlen(start);
        if ((size_t)(end - start) >= out_size)
            return 0;
        memcpy(out, start, (size_t)(end - start));
        out[end - start] = '\0';
        return 1;
    }

    return 0;
}

static void handle_line(char *line)
{
    char topic[96];
    char msg[128];

    if (!line || !line[0]) return;
    if (!get_query_value(line, "topic", topic, sizeof(topic))) return;
    if (!get_query_value(line, "msg", msg, sizeof(msg))) return;
    queue_device_message(topic, msg);
}

static void process_rx_buffer(char *buf, size_t *len)
{
    char *line_start = buf;
    char *newline;

    buf[*len] = '\0';
    while ((newline = strchr(line_start, '\n')) != NULL) {
        *newline = '\0';
        if (newline > line_start && *(newline - 1) == '\r')
            *(newline - 1) = '\0';
        handle_line(line_start);
        line_start = newline + 1;
    }

    *len = strlen(line_start);
    memmove(buf, line_start, *len);
}

static void *bemfa_thread(void *arg)
{
    (void)arg;

    while (g_running) {
        int sock = connect_bemfa();
        char rx[2048];
        size_t rx_len = 0;
        time_t last_heartbeat = 0;

        if (sock < 0) {
            sleep(BEMFA_RECONNECT_SEC);
            continue;
        }

        if (subscribe_topics(sock) < 0) {
            close(sock);
            sleep(BEMFA_RECONNECT_SEC);
            continue;
        }

        set_active_socket(sock);
        fprintf(stderr, "[Bemfa] connected and subscribed\n");

        while (g_running) {
            fd_set rfds;
            struct timeval tv;
            int ret;
            time_t now = time(NULL);

            if (now - last_heartbeat >= BEMFA_HEARTBEAT_SEC) {
                char heartbeat[128];
                snprintf(heartbeat, sizeof(heartbeat), "ping\r\n");
                pthread_mutex_lock(&g_sock_mutex);
                ret = socket_send_all(sock, heartbeat);
                pthread_mutex_unlock(&g_sock_mutex);
                if (ret < 0)
                    break;
                last_heartbeat = now;
            }

            FD_ZERO(&rfds);
            FD_SET(sock, &rfds);
            tv.tv_sec = 1;
            tv.tv_usec = 0;
            ret = select(sock + 1, &rfds, NULL, NULL, &tv);
            if (ret < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (ret == 0) continue;
            if (FD_ISSET(sock, &rfds)) {
                ssize_t n = recv(sock, rx + rx_len, sizeof(rx) - rx_len - 1, 0);
                if (n <= 0) break;
                rx_len += (size_t)n;
                if (rx_len >= sizeof(rx) - 1)
                    rx_len = 0;
                process_rx_buffer(rx, &rx_len);
            }
        }

        set_active_socket(-1);
        close(sock);
        if (g_running)
            sleep(BEMFA_RECONNECT_SEC);
    }

    return NULL;
}

void bemfa_start(const char *uid)
{
    const char *env_uid;

    if (g_running) return;

    env_uid = getenv("BEMFA_UID");
    if (uid && uid[0]) {
        strncpy(g_uid, uid, sizeof(g_uid) - 1);
    } else if (env_uid && env_uid[0]) {
        strncpy(g_uid, env_uid, sizeof(g_uid) - 1);
    }
    g_uid[sizeof(g_uid) - 1] = '\0';

    if (!g_uid[0] || strcmp(g_uid, "PUT_YOUR_BEMFA_UID_HERE") == 0) {
        fprintf(stderr, "[Bemfa] UID not configured, skip cloud connection\n");
        return;
    }

    g_running = 1;
    if (pthread_create(&g_thread, NULL, bemfa_thread, NULL) != 0) {
        g_running = 0;
        fprintf(stderr, "[Bemfa] failed to start thread\n");
    }
}

void bemfa_stop(void)
{
    if (!g_running) return;
    g_running = 0;
    pthread_join(g_thread, NULL);
}
