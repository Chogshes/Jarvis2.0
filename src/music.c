#include "music.h"
#include "audio_output.h"
#include "lyrics_parser.h"
#include "ui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LV_FONT_DECLARE(cjk_font_20);

// ── Playlist ──────────────────────────────────────────────
#define DEFAULT_PLS_COUNT 4
static const char *g_default_paths[DEFAULT_PLS_COUNT] = {
    "/Mydata/music/1.mp3", "/Mydata/music/2.mp3",
    "/Mydata/music/3.mp3", "/Mydata/music/4.mp3"
};
static const char **g_paths     = g_default_paths;
static int          g_pls_count = DEFAULT_PLS_COUNT;
static int          g_cur       = 0;

// ── State ─────────────────────────────────────────────────
static int g_state      = MUSIC_STATE_STOPPED;
static int g_volume     = 95;
static int g_slider_dragging = 0;
static int g_last_idx   = -1;
static lv_timer_t *g_timer = NULL;

// ── Lyrics buffer ─────────────────────────────────────────
#define LYR_N 7
static char g_lyric_buf[4096];

// ── Helpers ───────────────────────────────────────────────
static void fmt_time(int ms, char *buf, int sz)
{
    int m = ms / 60000, s = (ms % 60000) / 1000;
    snprintf(buf, sz, "%d:%02d", m, s);
}

// ── Load song metadata / lyrics / art, then start playback ─
static void load_and_play(int idx)
{
    if (idx < 0 || idx >= g_pls_count) return;

    // Load timed lyrics via parser
    lyrics_load_from_mp3(g_paths[idx]);
    g_last_idx = -1;

    // Read LRC for metadata (first 3 lines) and duration (last line)
    char lrc_path[256];
    snprintf(lrc_path, sizeof(lrc_path), "/Mydata/music/%d.lrc", idx + 1);

    char meta[256]  = "";
    char last_line[512] = "";
    FILE *f = fopen(lrc_path, "r");
    if (f) {
        for (int i = 0; i < 3; i++) {
            char line[512];
            if (!fgets(line, sizeof(line), f)) break;
            int len = strlen(line);
            while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
                line[--len] = '\0';
            char *p = line;
            while (*p == '[') {
                char *e = strchr(p, ']');
                if (e) p = e + 1; else break;
            }
            if (*p) {
                if (i > 0) strncat(meta, "\n", sizeof(meta) - strlen(meta) - 1);
                strncat(meta, p, sizeof(meta) - strlen(meta) - 1);
            }
        }
        // Scan last non-empty line
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            int l = strlen(line);
            while (l > 0 && (line[l-1] == '\n' || line[l-1] == '\r'))
                line[--l] = '\0';
            if (l > 0) strncpy(last_line, line, sizeof(last_line) - 1);
        }
        fclose(f);
    }

    lv_textarea_set_text(ui_songerText, meta[0] ? meta : "Track info");

    // Parse duration from last line tag, e.g. [03:45.00]end
    int dur = 0;
    if (strstr(last_line, "end")) {
        int min = 0, sec = 0, ms = 0;
        if (sscanf(last_line, "[%d:%d.%d]", &min, &sec, &ms) >= 2) {
            dur = min * 60000 + sec * 1000 + ms;
        }
    }
    if (dur <= 0) {
        // LRC 没有 end 标签 → 从 MP3 文件大小估算时长 (假设 128kbps)
        FILE *mp3 = fopen(g_paths[idx], "rb");
        if (mp3) {
            fseek(mp3, 0, SEEK_END);
            long fsize = ftell(mp3);
            fclose(mp3);
            if (fsize > 0) dur = (int)(fsize * 8LL / 128000 * 1000);
        }
    }
    if (dur > 0) {
        audio_set_duration(dur);
        char tbuf[16];
        fmt_time(dur, tbuf, sizeof(tbuf));
        lv_label_set_text(ui_sumtimeLabel3, tbuf);
    } else {
        lv_label_set_text(ui_sumtimeLabel3, "0:00");
    }

    // Reset UI
    lv_slider_set_value(ui_timeSlider3, 0, LV_ANIM_OFF);
    lv_label_set_text(ui_nowtimeLabel3, "0:00");
    g_slider_dragging = 0;
    lv_obj_scroll_to_y(ui_songerText, 0, LV_ANIM_OFF);
    lv_textarea_set_text(ui_musicTextArea, "");

    // Album art
    char bmp[256];
    snprintf(bmp, sizeof(bmp), "S:/Mydata/music/%d.bmp", idx + 1);
    lv_img_set_src(ui_musiceCordImage, bmp);

    // Start audio playback
    g_state = MUSIC_STATE_PLAYING;
    audio_play(g_paths[idx]);
}

// ── Timer callback ────────────────────────────────────────
static void timer_cb(lv_timer_t *t)
{
    (void)t;
    if (g_state == MUSIC_STATE_STOPPED) return;

    // If playback ended naturally, stop the UI timer
    if (g_state == MUSIC_STATE_PLAYING && !audio_is_playing() && !audio_is_paused()) {
        music_stop();
        return;
    }

    int cur = audio_get_time_ms();
    int dur = audio_get_duration_ms();

    // Progress slider — skip while dragging or waiting for seek
    if (dur > 0 && ui_timeSlider3) {
        if (g_slider_dragging > 0) g_slider_dragging--;
        if (!g_slider_dragging)
            lv_slider_set_value(ui_timeSlider3, cur * 100 / dur, LV_ANIM_OFF);
    }

    char buf[16];
    fmt_time(cur, buf, sizeof(buf));
    if (ui_nowtimeLabel3) lv_label_set_text(ui_nowtimeLabel3, buf);
    if (dur > 0) {
        fmt_time(dur, buf, sizeof(buf));
        if (ui_sumtimeLabel3) lv_label_set_text(ui_sumtimeLabel3, buf);
    }

    // Lyrics display — write directly into musicTextArea
    if (!lyrics_has_loaded()) return;

    int idx = lyrics_get_current_line_index(cur);
    if (idx < 0) return;
    if (idx == g_last_idx && cur > 0) return;
    g_last_idx = idx;

    int total = lyrics_get_total_count();
    int start = idx - LYR_N / 2;
    if (start < 0) start = 0;

    int pos = 0;
    for (int i = 0; i < LYR_N && start + i < total; i++) {
        const char *ln = lyrics_get_line(start + i);
        if (!ln) ln = "";
        if (start + i == idx)
            pos += snprintf(g_lyric_buf + pos, sizeof(g_lyric_buf) - pos,
                            "> %s\n", ln);
        else
            pos += snprintf(g_lyric_buf + pos, sizeof(g_lyric_buf) - pos,
                            "  %s\n", ln);
    }
    if (ui_musicTextArea)
        lv_textarea_set_text(ui_musicTextArea, g_lyric_buf);
}

// ── Public API ────────────────────────────────────────────

void music_init(void)
{
    g_state    = MUSIC_STATE_STOPPED;
    g_cur      = 0;
    g_volume   = 95;
    g_slider_dragging = 0;
    g_last_idx = -1;
    audio_set_volume(g_volume);

    // Set CJK font on Chinese text controls (with NULL guard)
    if (ui_musicTextArea)
        lv_obj_set_style_text_font(ui_musicTextArea, &cjk_font_20, LV_PART_MAIN);
    if (ui_songerText)
        lv_obj_set_style_text_font(ui_songerText, &cjk_font_20, LV_PART_MAIN);

    // Set initial button image to start (stopped state)
    if (ui_stopButt)
        lv_imgbtn_set_src(ui_stopButt, LV_IMGBTN_STATE_RELEASED, NULL,
                          &ui_img_start_png, NULL);

    if (!g_timer)
        g_timer = lv_timer_create(timer_cb, 300, NULL);
}

void music_deinit(void)
{
    music_stop();
    if (g_timer) {
        lv_timer_del(g_timer);
        g_timer = NULL;
    }
}

void music_set_playlist(const char *paths[], int count)
{
    if (!paths || count <= 0 || count > MUSIC_MAX_PLAYLIST) return;
    g_paths     = paths;
    g_pls_count = count;
    g_cur       = 0;
}

int music_play(int index)
{
    if (index < 0 || index >= g_pls_count) return -1;
    g_cur = index;
    load_and_play(g_cur);
    return 0;
}

void music_stop(void)
{
    audio_stop();
    g_state    = MUSIC_STATE_STOPPED;
    g_last_idx = -1;
    lyrics_reset();

    lv_slider_set_value(ui_timeSlider3, 0, LV_ANIM_OFF);
    lv_label_set_text(ui_nowtimeLabel3, "0:00");
    lv_label_set_text(ui_sumtimeLabel3, "0:00");

}

void music_pause(void)
{
    if (g_state != MUSIC_STATE_PLAYING) return;
    audio_pause();
    g_state = MUSIC_STATE_PAUSED;
}

void music_resume(void)
{
    if (g_state != MUSIC_STATE_PAUSED) return;
    audio_resume();
    g_state = MUSIC_STATE_PLAYING;
}

void music_toggle_pause(void)
{
    if (g_state == MUSIC_STATE_PLAYING)
        music_pause();
    else if (g_state == MUSIC_STATE_PAUSED)
        music_resume();
    else if (g_state == MUSIC_STATE_STOPPED)
        music_play(g_cur);
}

void music_next(void)
{
    if (g_pls_count == 0) return;
    g_cur = (g_cur + 1) % g_pls_count;
    load_and_play(g_cur);
}

void music_prev(void)
{
    if (g_pls_count == 0) return;
    g_cur = (g_cur - 1 + g_pls_count) % g_pls_count;
    load_and_play(g_cur);
}

void music_set_volume(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    g_volume = pct;
    audio_set_volume(pct);
}

int music_get_state(void)          { return g_state; }
int music_get_current_index(void)  { return g_cur; }
int music_get_volume(void)         { return g_volume; }
int music_get_playlist_count(void) { return g_pls_count; }

// ── UI event handlers ─────────────────────────────────────

void music_on_play_pause_btn(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    music_toggle_pause();
    int s = music_get_state();
    lv_imgbtn_set_src(btn, LV_IMGBTN_STATE_RELEASED, NULL,
        s == MUSIC_STATE_PLAYING ? &ui_img_stop_png : &ui_img_start_png, NULL);
}

void music_on_next_btn(lv_event_t *e)
{
    (void)e;
    music_next();
}

void music_on_prev_btn(lv_event_t *e)
{
    (void)e;
    music_prev();
}

void music_on_volume_slider(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    int vol = lv_slider_get_value(slider);
    music_set_volume(vol);
}

void music_on_time_slider(lv_event_t *e)
{
    if (!e) return;
    int dur = audio_get_duration_ms();
    if (dur <= 0) return;
    int pct = lv_slider_get_value(ui_timeSlider3);
    int target = dur * pct / 100;
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_VALUE_CHANGED) {
        g_slider_dragging = 1;
        char buf[16];
        fmt_time(target, buf, sizeof(buf));
        lv_label_set_text(ui_nowtimeLabel3, buf);
    } else if (code == LV_EVENT_RELEASED) {
        audio_seek(target);
        g_slider_dragging = 5;
    }
}
