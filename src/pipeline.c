#include "pipeline.h"
#include "ui.h"
#include "ui_events.h"
#include "music.h"
#include "video.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

lv_timer_t *g_pipeline_timer = NULL;
lv_timer_t *g_control_timer = NULL;
int g_has_intent = 0;
int g_step = 1;
int g_tick = 0;
char g_asr_text[512] = {0};
char g_ai_reply[2048] = {0};

static pthread_mutex_t g_llm_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_llm_done = 0;
static int g_pending_has_intent = 0;
static Intent g_pending_intent;

static void change_screen(lv_obj_t **screen, void (*init)(void));

static int contains_any(const char *text, const char *const words[], int count)
{
    if (!text) return 0;
    for (int i = 0; i < count; i++) {
        if (words[i] && strstr(text, words[i])) return 1;
    }
    return 0;
}

static int is_light_device(int device)
{
    return device == HOME_DEVICE_MASTER_LIGHT ||
           device == HOME_DEVICE_LIVING_LIGHT ||
           device == HOME_DEVICE_KITCHEN_LIGHT;
}

static int clamp_command_value(int value, int min_value, int max_value)
{
    if (value < min_value) return min_value;
    if (value > max_value) return max_value;
    return value;
}

static const char *home_device_name(int device)
{
    switch (device) {
    case HOME_DEVICE_THERMOMETER: return "调温计";
    case HOME_DEVICE_AC: return "空调";
    case HOME_DEVICE_AIR_PURIFIER: return "空气净化器";
    case HOME_DEVICE_MASTER_LIGHT: return "主卧灯";
    case HOME_DEVICE_LIVING_LIGHT: return "客厅灯";
    case HOME_DEVICE_KITCHEN_LIGHT: return "厨房灯";
    case HOME_DEVICE_REFRIGERATOR: return "冰箱";
    case HOME_DEVICE_WASHER: return "洗衣机";
    case HOME_DEVICE_CURTAIN: return "窗帘";
    default: return "设备";
    }
}

static int detect_home_device(const char *text)
{
    const char *const thermometer_words[] = {"调温计", "温度计", "温控", "thermometer"};
    const char *const ac_words[] = {"空调", "冷气", "ac unit", "air conditioner"};
    const char *const purifier_words[] = {"空气净化器", "净化器", "air purifier", "purifier"};
    const char *const refrigerator_words[] = {"冰箱", "fridge", "refrigerator"};
    const char *const washer_words[] = {"洗衣机", "washer", "wash machine"};
    const char *const curtain_words[] = {"窗帘", "curtain"};

    if (!text) return -1;
    if (contains_any(text, curtain_words, (int)(sizeof(curtain_words) / sizeof(curtain_words[0]))))
        return HOME_DEVICE_CURTAIN;
    if (contains_any(text, refrigerator_words, (int)(sizeof(refrigerator_words) / sizeof(refrigerator_words[0]))))
        return HOME_DEVICE_REFRIGERATOR;
    if (contains_any(text, washer_words, (int)(sizeof(washer_words) / sizeof(washer_words[0]))))
        return HOME_DEVICE_WASHER;
    if (contains_any(text, purifier_words, (int)(sizeof(purifier_words) / sizeof(purifier_words[0]))))
        return HOME_DEVICE_AIR_PURIFIER;
    if (contains_any(text, ac_words, (int)(sizeof(ac_words) / sizeof(ac_words[0]))))
        return HOME_DEVICE_AC;
    if (contains_any(text, thermometer_words, (int)(sizeof(thermometer_words) / sizeof(thermometer_words[0]))))
        return HOME_DEVICE_THERMOMETER;
    if (strstr(text, "灯") || strstr(text, "light")) {
        if (strstr(text, "主卧") || strstr(text, "卧室") || strstr(text, "master"))
            return HOME_DEVICE_MASTER_LIGHT;
        if (strstr(text, "客厅") || strstr(text, "living"))
            return HOME_DEVICE_LIVING_LIGHT;
        if (strstr(text, "厨房") || strstr(text, "kitchen"))
            return HOME_DEVICE_KITCHEN_LIGHT;
    }
    return -1;
}

static void chinese_number_text(int value, char *buf, size_t buf_size)
{
    const char *const digits[] = {"零", "一", "二", "三", "四", "五", "六", "七", "八", "九"};
    if (value <= 10) {
        const char *const ten_digits[] = {"零", "一", "二", "三", "四", "五", "六", "七", "八", "九", "十"};
        snprintf(buf, buf_size, "%s", ten_digits[value]);
    } else if (value < 20) {
        snprintf(buf, buf_size, "十%s", digits[value % 10]);
    } else if (value < 100) {
        int tens = value / 10;
        int ones = value % 10;
        if (ones == 0)
            snprintf(buf, buf_size, "%s十", digits[tens]);
        else
            snprintf(buf, buf_size, "%s十%s", digits[tens], digits[ones]);
    } else {
        snprintf(buf, buf_size, "一百");
    }
}

static int extract_spoken_number(const char *text, int *value)
{
    if (!text || !value) return 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (isdigit(*p)) {
            *value = atoi((const char *)p);
            return 1;
        }
    }
    for (int i = 100; i >= 0; i--) {
        char number_text[32];
        chinese_number_text(i, number_text, sizeof(number_text));
        if (strstr(text, number_text)) {
            *value = i;
            return 1;
        }
    }
    if (strstr(text, "两")) {
        *value = 2;
        return 1;
    }
    return 0;
}

static int handle_home_device_command(const char *text, char *status, size_t status_size)
{
    const char *const off_words[] = {"关闭", "关掉", "关上", "关", "stop", "off"};
    const char *const on_words[] = {"打开", "开启", "启动", "开", "调到", "调成", "亮度", "on"};
    int value = 0;
    int has_value = extract_spoken_number(text, &value);
    int device = detect_home_device(text);
    int wants_off = contains_any(text, off_words, (int)(sizeof(off_words) / sizeof(off_words[0])));
    int wants_on = contains_any(text, on_words, (int)(sizeof(on_words) / sizeof(on_words[0]))) || has_value;
    const char *name;

    if (device < 0) return 0;

    if (lv_scr_act() != ui_houseScreen)
        change_screen(&ui_houseScreen, ui_houseScreen_screen_init);
    name = home_device_name(device);

    if (wants_off && !has_value) {
        home_set_device_power(device, 0);
        snprintf(status, status_size, "已关闭%s。", name);
        return 1;
    }

    if (device == HOME_DEVICE_AC) {
        int temperature = has_value ? clamp_command_value(value, 16, 35) : 25;
        home_set_ac_temperature(temperature);
        snprintf(status, status_size, "已将%s调到%d度。", name, temperature);
        return 1;
    }

    if (is_light_device(device)) {
        int brightness = has_value ? clamp_command_value(value, 0, 100) : 20;
        home_set_light_brightness(device, brightness);
        snprintf(status, status_size, "已将%s亮度调到%d%%。", name, brightness);
        return 1;
    }

    if (!wants_on && wants_off) {
        home_set_device_power(device, 0);
        snprintf(status, status_size, "已关闭%s。", name);
        return 1;
    }

    home_set_device_power(device, 1);
    snprintf(status, status_size, "已打开%s。", name);
    return 1;
}

static void change_screen(lv_obj_t **screen, void (*init)(void))
{
    _ui_screen_change(screen, LV_SCR_LOAD_ANIM_FADE_ON, 300, 0, init);
}

static void append_ai_status(const char *msg)
{
    lv_textarea_add_text(ui_ansText, "AI: ");
    lv_textarea_add_text(ui_ansText, msg);
    lv_textarea_add_text(ui_ansText, "\n");
    lv_textarea_set_placeholder_text(ui_speakText, "Type or speak a command...");
}

static int handle_local_command(const char *text, char *status, size_t status_size)
{
    const char *const music_words[] = {"音乐", "歌曲", "歌", "music"};
    const char *const video_words[] = {"视频", "影片", "电影", "video"};
    const char *const home_words[] = {"家居", "智能家居", "设备", "house"};
    const char *const main_words[] = {"主页", "首页", "主界面", "main"};
    const char *const play_words[] = {"播放", "开始", "打开", "放"};
    const char *const pause_words[] = {"暂停", "停一下"};
    const char *const stop_words[] = {"停止", "关闭", "关掉"};
    const char *const resume_words[] = {"继续", "恢复"};
    const char *const next_words[] = {"下一首", "下一曲", "下一集", "下一个"};
    const char *const prev_words[] = {"上一首", "上一曲", "上一集", "上一个"};
    const char *const episode_words[] = {"下一集", "上一集", "剧集"};

    int is_music = contains_any(text, music_words, (int)(sizeof(music_words) / sizeof(music_words[0])));
    int is_video = contains_any(text, video_words, (int)(sizeof(video_words) / sizeof(video_words[0])));
    int is_home = contains_any(text, home_words, (int)(sizeof(home_words) / sizeof(home_words[0])));
    int is_main = contains_any(text, main_words, (int)(sizeof(main_words) / sizeof(main_words[0])));
    int is_play = contains_any(text, play_words, (int)(sizeof(play_words) / sizeof(play_words[0])));
    int is_pause = contains_any(text, pause_words, (int)(sizeof(pause_words) / sizeof(pause_words[0])));
    int is_stop = contains_any(text, stop_words, (int)(sizeof(stop_words) / sizeof(stop_words[0])));
    int is_resume = contains_any(text, resume_words, (int)(sizeof(resume_words) / sizeof(resume_words[0])));
    int is_next = contains_any(text, next_words, (int)(sizeof(next_words) / sizeof(next_words[0])));
    int is_prev = contains_any(text, prev_words, (int)(sizeof(prev_words) / sizeof(prev_words[0])));
    if (contains_any(text, episode_words, (int)(sizeof(episode_words) / sizeof(episode_words[0]))))
        is_video = 1;

    if (handle_home_device_command(text, status, status_size)) {
        return 1;
    }

    if (is_main) {
        change_screen(&ui_mainScreen, ui_mainScreen_screen_init);
        snprintf(status, status_size, "已切换到主页。");
        return 1;
    }

    if (is_home) {
        change_screen(&ui_houseScreen, ui_houseScreen_screen_init);
        snprintf(status, status_size, "已切换到家居界面。");
        return 1;
    }

    if (is_music) {
        change_screen(&ui_musicScreen, ui_musicScreen_screen_init);
        if (is_next) {
            music_next();
            snprintf(status, status_size, "已播放下一首音乐。");
        } else if (is_prev) {
            music_prev();
            snprintf(status, status_size, "已播放上一首音乐。");
        } else if (is_stop) {
            music_stop();
            snprintf(status, status_size, "音乐已停止。");
        } else if (is_pause) {
            music_pause();
            snprintf(status, status_size, "音乐已暂停。");
        } else if (is_resume) {
            music_resume();
            snprintf(status, status_size, "音乐已继续播放。");
        } else {
            if (is_play || music_get_state() == MUSIC_STATE_STOPPED)
                music_play(music_get_current_index());
            else
                music_resume();
            snprintf(status, status_size, "已切换到音乐界面并开始播放。");
        }
        return 1;
    }

    if (is_video) {
        change_screen(&ui_videoScreen, ui_videoScreen_screen_init);
        if (is_next) {
            video_next();
            snprintf(status, status_size, "已播放下一集视频。");
        } else if (is_prev) {
            video_prev();
            snprintf(status, status_size, "已播放上一集视频。");
        } else if (is_stop) {
            video_stop();
            snprintf(status, status_size, "视频已停止。");
        } else if (is_pause) {
            video_pause();
            snprintf(status, status_size, "视频已暂停。");
        } else if (is_resume) {
            video_resume();
            snprintf(status, status_size, "视频已继续播放。");
        } else {
            if (is_play || (!video_is_playing() && !video_is_paused()))
                video_play_current();
            else
                video_resume();
            snprintf(status, status_size, "已切换到视频界面并开始播放。");
        }
        return 1;
    }

    if (is_next) {
        if (lv_scr_act() == ui_videoScreen) {
            video_next();
            snprintf(status, status_size, "已播放下一集视频。");
        } else {
            music_next();
            snprintf(status, status_size, "已播放下一首音乐。");
        }
        return 1;
    }

    if (is_prev) {
        if (lv_scr_act() == ui_videoScreen) {
            video_prev();
            snprintf(status, status_size, "已播放上一集视频。");
        } else {
            music_prev();
            snprintf(status, status_size, "已播放上一首音乐。");
        }
        return 1;
    }

    if (is_stop) {
        if (lv_scr_act() == ui_videoScreen) {
            video_stop();
            snprintf(status, status_size, "视频已停止。");
        } else if (lv_scr_act() == ui_musicScreen) {
            music_stop();
            snprintf(status, status_size, "音乐已停止。");
        } else {
            return 0;
        }
        return 1;
    }

    if (is_pause) {
        if (lv_scr_act() == ui_videoScreen) {
            video_pause();
            snprintf(status, status_size, "视频已暂停。");
        } else if (lv_scr_act() == ui_musicScreen) {
            music_pause();
            snprintf(status, status_size, "音乐已暂停。");
        } else {
            return 0;
        }
        return 1;
    }

    if (is_resume || is_play) {
        if (lv_scr_act() == ui_videoScreen) {
            if (video_is_paused()) video_resume(); else video_play_current();
            snprintf(status, status_size, "视频已播放。");
        } else if (lv_scr_act() == ui_musicScreen) {
            if (music_get_state() == MUSIC_STATE_PAUSED) music_resume();
            else if (music_get_state() == MUSIC_STATE_STOPPED) music_play(music_get_current_index());
            snprintf(status, status_size, "音乐已播放。");
        } else {
            return 0;
        }
        return 1;
    }

    return 0;
}

static void finish_pipeline_timer(void)
{
    if (g_pipeline_timer) {
        lv_timer_del(g_pipeline_timer);
        g_pipeline_timer = NULL;
    }
}

void pipeline_reset_llm_state(void)
{
    pthread_mutex_lock(&g_llm_mutex);
    g_llm_done = 0;
    g_pending_has_intent = 0;
    memset(&g_pending_intent, 0, sizeof(g_pending_intent));
    g_ai_reply[0] = '\0';
    pthread_mutex_unlock(&g_llm_mutex);
}

void on_asr_result(const char *text)
{
    if (!text) {
        return;
    }

    pthread_mutex_lock(&g_llm_mutex);
    strncpy(g_asr_text, text, sizeof(g_asr_text) - 1);
    g_asr_text[sizeof(g_asr_text) - 1] = '\0';
    pthread_mutex_unlock(&g_llm_mutex);
}

void on_deepseek_result(const Intent *intent, const char *reply)
{
    pthread_mutex_lock(&g_llm_mutex);

    if (reply && reply[0]) {
        strncpy(g_ai_reply, reply, sizeof(g_ai_reply) - 1);
        g_ai_reply[sizeof(g_ai_reply) - 1] = '\0';
    } else {
        strncpy(g_ai_reply, "LLM request failed.", sizeof(g_ai_reply) - 1);
        g_ai_reply[sizeof(g_ai_reply) - 1] = '\0';
    }

    if (intent) {
        g_pending_intent = *intent;
        g_pending_has_intent = 1;
    } else {
        memset(&g_pending_intent, 0, sizeof(g_pending_intent));
        g_pending_has_intent = 0;
    }

    g_llm_done = 1;
    pthread_mutex_unlock(&g_llm_mutex);
}

void process_pipeline(lv_timer_t *t)
{
    (void)t;

    g_tick++;

    if (g_step == 0) {
        char text[sizeof(g_asr_text)];

        pthread_mutex_lock(&g_llm_mutex);
        strncpy(text, g_asr_text, sizeof(text) - 1);
        text[sizeof(text) - 1] = '\0';
        pthread_mutex_unlock(&g_llm_mutex);

        if (text[0] != '\0') {
            // 语音识别完成 → 自动发送到 LLM，无需手动点击
            lv_textarea_set_text(ui_speakText, "");
            lv_textarea_add_text(ui_ansText, "You: ");
            lv_textarea_add_text(ui_ansText, text);
            lv_textarea_add_text(ui_ansText, "\n");
            lv_textarea_set_placeholder_text(ui_speakText, "Thinking...");

            pipeline_reset_llm_state();
            g_step = 1;    // 转到等待 LLM 回复
            g_tick = 0;
        } else if (g_tick >= 150) {
            lv_textarea_set_placeholder_text(ui_speakText, "Type or speak a command...");
            finish_pipeline_timer();
        }
        return;
    }

    if (g_step != 1) {
        return;
    }

    if (g_tick == 1) {
        char cmd[sizeof(g_asr_text)];
        char status[128];

        strncpy(cmd, g_asr_text, sizeof(cmd) - 1);
        cmd[sizeof(cmd) - 1] = '\0';
        if (handle_local_command(cmd, status, sizeof(status))) {
            append_ai_status(status);
            finish_pipeline_timer();
            return;
        }

        deepseek_send_request_async(cmd, on_deepseek_result);
    }

    char reply[sizeof(g_ai_reply)];
    int done;
    int has_intent;

    pthread_mutex_lock(&g_llm_mutex);
    done = g_llm_done;
    has_intent = g_pending_has_intent;
    strncpy(reply, g_ai_reply, sizeof(reply) - 1);
    reply[sizeof(reply) - 1] = '\0';
    pthread_mutex_unlock(&g_llm_mutex);

    if (!done) {
        if (g_tick >= 150) {
            lv_textarea_add_text(ui_ansText, "AI: LLM request timeout.\n");
            lv_textarea_set_placeholder_text(ui_speakText, "Type or speak a command...");
            finish_pipeline_timer();
        }
        return;
    }

    if (reply[0] == '\0') {
        strncpy(reply, "LLM returned an empty reply.", sizeof(reply) - 1);
        reply[sizeof(reply) - 1] = '\0';
    }

    lv_textarea_add_text(ui_ansText, "AI: ");
    lv_textarea_add_text(ui_ansText, reply);
    lv_textarea_add_text(ui_ansText, "\n");
    lv_textarea_set_placeholder_text(ui_speakText, "Type or speak a command...");

    if (has_intent) {
        g_has_intent = 1;
    }

    finish_pipeline_timer();
}
