// video.c —— pl_mpeg 解码 + ring buffer 音频 + LVGL canvas 渲染
//
// 架构说明：
//   解码线程 → on_audio() 非阻塞 push → ring buffer → 音频线程 阻塞 write OSS
//   解码线程 → on_video() 写 g_frame → UI 定时器 → LVGL canvas 渲染
//   播放器 UI 事件直接在视频模块内处理

#include "video.h"
#include "ui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/soundcard.h>
#define PL_MPEG_IMPLEMENTATION
#include "pl_mpeg.h"

LV_FONT_DECLARE(cjk_font_20);

void audio_stop(void);

// ═══════════════════════════════════════════════════════════
//  Ring buffer (SPSC, lock-free)
// ═══════════════════════════════════════════════════════════
#define RING_MASK  0x7FFF
#define RING_SIZE  (RING_MASK + 1)

static short          g_ring[RING_SIZE];
static volatile int   g_ring_w = 0;
static volatile int   g_ring_r = 0;
static pthread_t      g_audio_tid;
static volatile int   g_audio_alive = 0;
static volatile int   g_audio_stop  = 0;
static int            g_audio_rate  = 44100;

static int ring_available(void) { return (g_ring_w - g_ring_r) & RING_MASK; }

static void ring_clear(void)
{
    g_ring_w = 0;
    g_ring_r = 0;
}

static void ring_push(const short *data, int n)
{
    for (int i = 0; i < n; i++) {
        int next = (g_ring_w + 1) & RING_MASK;
        if (next == g_ring_r) return;
        g_ring[g_ring_w] = data[i];
        g_ring_w = next;
    }
}

static int ring_pop(short *buf, int max)
{
    int n = 0;
    while (n < max && g_ring_r != g_ring_w) {
        buf[n++] = g_ring[g_ring_r];
        g_ring_r = (g_ring_r + 1) & RING_MASK;
    }
    return n;
}

static void *audio_thread(void *arg)
{
    (void)arg;
    int dsp = open("/dev/dsp", O_WRONLY);
    if (dsp < 0) dsp = open("/dev/dsp1", O_WRONLY);
    if (dsp < 0) { g_audio_alive = 0; return NULL; }

    int fmt = AFMT_U16_LE, ch = 2, rate = g_audio_rate;
    ioctl(dsp, SNDCTL_DSP_SETFMT,    &fmt);
    ioctl(dsp, SNDCTL_DSP_CHANNELS,  &ch);
    ioctl(dsp, SNDCTL_DSP_SPEED,     &rate);

    short buf[512];
    while (!g_audio_stop) {
        int n = ring_pop(buf, 512);
        if (n > 0) {
            write(dsp, buf, n * sizeof(short));
        } else {
            usleep(5000);
        }
    }
    while (ring_available() > 0) {
        int n = ring_pop(buf, 512);
        if (n > 0) write(dsp, buf, n * sizeof(short));
    }
    close(dsp);
    g_audio_alive = 0;
    return NULL;
}

static void audio_thread_start(void)
{
    if (g_audio_alive) return;
    g_ring_w = 0; g_ring_r = 0;
    g_audio_stop  = 0;
    g_audio_alive = 1;
    pthread_create(&g_audio_tid, NULL, audio_thread, NULL);
    pthread_detach(g_audio_tid);
}

static void audio_thread_stop(void)
{
    if (!g_audio_alive) return;
    g_audio_stop = 1;
    for (int i = 0; i < 40 && g_audio_alive; i++) usleep(50000);
}

// ═══════════════════════════════════════════════════════════
//  低层播放器 (pl_mpeg 封装)
// ═══════════════════════════════════════════════════════════

static volatile int g_playing    = 0;
static volatile int g_paused     = 0;
static volatile int g_stop       = 0;
static volatile int g_alive      = 0;
static volatile int g_low_volume = 100;

static volatile double g_current_time = 0;
static volatile double g_duration     = 0;
static volatile double g_seek_target  = -1;
static volatile int    g_seek_in_progress = 0;

static volatile int g_width  = 0;
static volatile int g_height = 0;
static volatile int g_frame_ready = 0;

static pthread_t       g_thread;
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint8_t        *g_frame = NULL;

static double take_seek_target(void)
{
    double target;
    pthread_mutex_lock(&g_mutex);
    target = g_seek_target;
    if (target >= 0) {
        g_seek_target = -1;
        g_seek_in_progress = 1;
    }
    pthread_mutex_unlock(&g_mutex);
    return target;
}

static void finish_seek(void)
{
    pthread_mutex_lock(&g_mutex);
    g_seek_in_progress = 0;
    pthread_mutex_unlock(&g_mutex);
}

static int seek_is_busy(void)
{
    int busy;
    pthread_mutex_lock(&g_mutex);
    busy = g_seek_in_progress || g_seek_target >= 0;
    pthread_mutex_unlock(&g_mutex);
    return busy;
}

static void on_audio(plm_t *pl, plm_samples_t *s, void *user)
{
    (void)pl; (void)user;
    if (!s || s->count <= 0) return;

    float gain = (float)g_low_volume / 100.f;
    short buf[1024];
    int out = 0, max = (int)(sizeof(buf) / sizeof(buf[0]));

    for (int i = 0; i < s->count * 2 && out < max; i += 2) {
        float f = s->interleaved[i] * gain;
        if (f > 1.0f) f = 1.0f; else if (f < -1.0f) f = -1.0f;
        buf[out++] = (short)(f * 32767.f) ^ 0x8000;
        buf[out++] = (short)(f * 32767.f) ^ 0x8000;
    }
    ring_push(buf, out);
}

static void on_video(plm_t *pl, plm_frame_t *f, void *user)
{
    (void)pl; (void)user;
    if (!f || !g_frame) return;
    pthread_mutex_lock(&g_mutex);
    plm_frame_to_rgba(f, g_frame, g_width * 4);
    g_frame_ready = 1;
    g_current_time = f->time;
    pthread_mutex_unlock(&g_mutex);
}

static void *th(void *arg)
{
    const char *path = (const char *)arg;
    plm_t *pl = plm_create_with_filename(path);
    if (!pl) { free(arg); g_alive = 0; g_playing = 0; return NULL; }

    plm_set_loop(pl, 0);
    g_width      = plm_get_width(pl);
    g_height     = plm_get_height(pl);
    g_duration   = plm_get_duration(pl);
    g_audio_rate = plm_get_samplerate(pl);

    pthread_mutex_lock(&g_mutex);
    free(g_frame);
    g_frame = malloc(g_width * g_height * 4);
    pthread_mutex_unlock(&g_mutex);

    plm_set_video_decode_callback(pl, on_video, NULL);
    plm_set_audio_decode_callback(pl, on_audio, NULL);

    audio_thread_start();

    int seek_skip = 0;
    while (g_playing && !g_stop) {
        double target = take_seek_target();
        if (target >= 0) {
            ring_clear();
            plm_seek(pl, target, 0);
            ring_clear();
            pthread_mutex_lock(&g_mutex);
            if (g_seek_target < 0) g_current_time = target;
            pthread_mutex_unlock(&g_mutex);
            finish_seek();
            seek_skip = 2;
            continue;
        }

        if (g_paused) {
            usleep(100000);
            continue;
        }

        if (seek_skip > 0) { seek_skip--; continue; }

        plm_decode(pl, 0.03);
        if (plm_has_ended(pl)) break;
    }

    audio_thread_stop();
    plm_destroy(pl);
    free(arg);
    g_alive   = 0;
    g_playing = 0;
    return NULL;
}

void video_play(const char *path)
{
    video_stop();
    audio_stop();

    char *copy = strdup(path);
    g_playing      = 1;
    g_paused       = 0;
    g_stop         = 0;
    g_seek_target  = -1;
    g_seek_in_progress = 0;
    g_current_time = 0;
    ring_clear();
    g_alive        = 1;
    pthread_create(&g_thread, NULL, th, copy);
}

void video_stop(void)
{
    if (!g_alive) return;
    g_stop    = 1;
    g_paused  = 0;
    g_playing = 0;
    pthread_join(g_thread, NULL);
    g_alive = 0;
}

void video_pause(void)                   { g_paused = 1; }
void video_resume(void)                  { g_paused = 0; }
int  video_is_playing(void)              { return g_playing && !g_paused; }
int  video_is_paused(void)               { return g_paused; }
double video_get_time(void)
{
    double t;
    pthread_mutex_lock(&g_mutex);
    t = g_current_time;
    pthread_mutex_unlock(&g_mutex);
    return t;
}
double video_get_duration(void)
{
    double d;
    pthread_mutex_lock(&g_mutex);
    d = g_duration;
    pthread_mutex_unlock(&g_mutex);
    return d;
}
void video_seek(double seconds)
{
    pthread_mutex_lock(&g_mutex);
    g_seek_target = seconds;
    g_current_time = seconds;
    pthread_mutex_unlock(&g_mutex);
}

void video_set_volume(int pct)
{
    if (pct < 0) pct = 0; else if (pct > 100) pct = 100;
    g_low_volume = pct;
}

int video_get_frame(uint8_t **buf, int *w, int *h)
{
    pthread_mutex_lock(&g_mutex);
    if (!g_frame_ready || !g_frame) { pthread_mutex_unlock(&g_mutex); return 0; }
    *w = g_width; *h = g_height; *buf = g_frame;
    pthread_mutex_unlock(&g_mutex);
    return 1;
}

// ═══════════════════════════════════════════════════════════
//  高层 UI 模块 (LVGL canvas 渲染 + 定时器 + 事件处理)
// ═══════════════════════════════════════════════════════════

#define VPLS 4
static const char *g_vpaths[VPLS] = {
    "/Mydata/video/1.mpg", "/Mydata/video/2.mpg",
    "/Mydata/video/3.mpg", "/Mydata/video/4.mpg"
};
static int g_vcur = 0;

static lv_obj_t    *g_video_canvas = NULL;
static lv_color_t  *g_canvas_buf   = NULL;
static int          g_canvas_w     = 0;
static int          g_canvas_h     = 0;
static lv_timer_t  *g_vtimer        = NULL;
static int          g_vdragging      = 0;
static int          g_panel2_hide_cnt = 0;

static void video_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!video_is_playing() && !video_is_paused()) return;
    if (!ui_videoContain) return;

    // Panel2 自动隐藏：3 秒无操作后消失
    if (ui_Panel2 && !lv_obj_has_flag(ui_Panel2, LV_OBJ_FLAG_HIDDEN)) {
        if (++g_panel2_hide_cnt > 100) {  // 100 × 30ms = 3s
            lv_obj_add_flag(ui_Panel2, LV_OBJ_FLAG_HIDDEN);
            g_panel2_hide_cnt = 0;
        }
    } else {
        g_panel2_hide_cnt = 0;
    }

    // 不在视频页面就自动暂停
    if (lv_scr_act() != lv_obj_get_screen(ui_videoContain)) {
        if (video_is_playing()) video_pause();
        return;
    }

    // 取出帧 → 渲染到 LVGL canvas
    uint8_t *src; int w, h;
    if (g_video_canvas && video_get_frame(&src, &w, &h)) {
        // 懒分配 / 重分配 canvas buffer
        if (!g_canvas_buf || g_canvas_w != w || g_canvas_h != h) {
            free(g_canvas_buf);
            g_canvas_buf = malloc(w * h * sizeof(lv_color_t));
            g_canvas_w = w; g_canvas_h = h;
            if (g_canvas_buf) {
                lv_canvas_set_buffer(g_video_canvas, g_canvas_buf, w, h, LV_IMG_CF_TRUE_COLOR);
                // 等比缩放适配容器
                lv_coord_t cw = lv_obj_get_width(ui_videoContain);
                lv_coord_t ch = lv_obj_get_height(ui_videoContain);
                float scale = ((float)w / cw > (float)h / ch) ? (float)cw / w : (float)ch / h;
                lv_obj_set_size(g_video_canvas, (lv_coord_t)(w * scale), (lv_coord_t)(h * scale));
                lv_obj_center(g_video_canvas);
            }
        }
        // RGBA → lv_color_t
        if (g_canvas_buf) {
            for (int i = 0; i < w * h; i++)
                g_canvas_buf[i] = lv_color_make(src[i*4], src[i*4+1], src[i*4+2]);
            lv_obj_invalidate(g_video_canvas);
        }
    }

    // 进度条
    int cur = (int)video_get_time(), dur = (int)video_get_duration();
    if (dur > 0 && ui_timeSlider) {
        if (g_vdragging > 0) g_vdragging--;
        if (!g_vdragging)
            lv_slider_set_value(ui_timeSlider, cur * 100 / dur, LV_ANIM_OFF);
    }
    char b[16];
    snprintf(b, sizeof(b), "%d:%02d", cur / 60, cur % 60);
    if (ui_nowtimeLabel) lv_label_set_text(ui_nowtimeLabel, b);
    if (dur > 0 && ui_sumtimeLabel) {
        snprintf(b, sizeof(b), "%d:%02d", dur / 60, dur % 60);
        lv_label_set_text(ui_sumtimeLabel, b);
    }
}

// ── 初始化 ──────────────────────────────────────────────

void video_module_init(void)
{
    if (!g_video_canvas && ui_videoContain) {
        g_video_canvas = lv_canvas_create(ui_videoContain);
        lv_obj_center(g_video_canvas);
    }
    if (ui_videoText)
        lv_obj_set_style_text_font(ui_videoText, &cjk_font_20, LV_PART_MAIN);

    if (!g_vtimer)
        g_vtimer = lv_timer_create(video_timer_cb, 30, NULL);
}

void video_module_deinit(void)
{
    if (g_vtimer) { lv_timer_del(g_vtimer); g_vtimer = NULL; }
    video_stop();
    if (g_video_canvas) { lv_obj_del(g_video_canvas); g_video_canvas = NULL; }
    free(g_canvas_buf); g_canvas_buf = NULL;
    g_canvas_w = 0; g_canvas_h = 0;
}

void video_render_frame(void) { /* 由定时器调用 */ }

// ── UI 事件处理 (由 ui_events.c 调用) ────────────────────

void video_on_play_pause_btn(lv_event_t *e)
{
    if (!e || lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    if (video_is_playing()) {
        video_pause();
        lv_imgbtn_set_src(ui_stopButt2, LV_IMGBTN_STATE_RELEASED, NULL,
                          &ui_img_start_png, NULL);
    } else if (video_is_paused()) {
        video_resume();
        lv_imgbtn_set_src(ui_stopButt2, LV_IMGBTN_STATE_RELEASED, NULL,
                          &ui_img_stop_png, NULL);
    } else {
        video_play(g_vpaths[g_vcur]);
        lv_imgbtn_set_src(ui_stopButt2, LV_IMGBTN_STATE_RELEASED, NULL,
                          &ui_img_stop_png, NULL);
    }

    // 读取视频标题（从 .txt 文件首行）
    char tpath[256];
    snprintf(tpath, sizeof(tpath), "/Mydata/video/%d.txt", g_vcur + 1);
    FILE *tf = fopen(tpath, "r");
    if (tf) {
        char tline[256];
        if (fgets(tline, sizeof(tline), tf)) {
            int len = strlen(tline);
            while (len > 0 && (tline[len-1]=='\n'||tline[len-1]=='\r')) tline[--len] = 0;
            lv_textarea_set_text(ui_videoText, tline);
        }
        fclose(tf);
    } else {
        const char *name = strrchr(g_vpaths[g_vcur], '/');
        lv_textarea_set_text(ui_videoText, name ? name + 1 : g_vpaths[g_vcur]);
    }
}

void video_on_next_btn(lv_event_t *e)
{
    if (!e || lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    video_stop();
    g_vcur = (g_vcur + 1) % VPLS;
    video_play(g_vpaths[g_vcur]);
    lv_imgbtn_set_src(ui_stopButt2, LV_IMGBTN_STATE_RELEASED, NULL,
                      &ui_img_stop_png, NULL);

    char tpath[256];
    snprintf(tpath, sizeof(tpath), "/Mydata/video/%d.txt", g_vcur + 1);
    FILE *tf = fopen(tpath, "r");
    if (tf) {
        char tline[256];
        if (fgets(tline, sizeof(tline), tf)) {
            int len = strlen(tline);
            while (len > 0 && (tline[len-1]=='\n'||tline[len-1]=='\r')) tline[--len] = 0;
            lv_textarea_set_text(ui_videoText, tline);
        }
        fclose(tf);
    } else {
        const char *name = strrchr(g_vpaths[g_vcur], '/');
        lv_textarea_set_text(ui_videoText, name ? name + 1 : g_vpaths[g_vcur]);
    }
}

void video_on_prev_btn(lv_event_t *e)
{
    if (!e || lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    video_stop();
    g_vcur = (g_vcur - 1 + VPLS) % VPLS;
    video_play(g_vpaths[g_vcur]);
    lv_imgbtn_set_src(ui_stopButt2, LV_IMGBTN_STATE_RELEASED, NULL,
                      &ui_img_stop_png, NULL);

    char tpath[256];
    snprintf(tpath, sizeof(tpath), "/Mydata/video/%d.txt", g_vcur + 1);
    FILE *tf = fopen(tpath, "r");
    if (tf) {
        char tline[256];
        if (fgets(tline, sizeof(tline), tf)) {
            int len = strlen(tline);
            while (len > 0 && (tline[len-1]=='\n'||tline[len-1]=='\r')) tline[--len] = 0;
            lv_textarea_set_text(ui_videoText, tline);
        }
        fclose(tf);
    } else {
        const char *name = strrchr(g_vpaths[g_vcur], '/');
        lv_textarea_set_text(ui_videoText, name ? name + 1 : g_vpaths[g_vcur]);
    }
}

void video_on_volume_slider(lv_event_t *e)
{
    if (!e || lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    int v = lv_slider_get_value(ui_Slider4);
    video_set_volume(v);
    char b[16];
    snprintf(b, sizeof(b), "%d%%", v);
    lv_label_set_text(ui_soundLabel2, b);
}

void video_on_time_slider(lv_event_t *e)
{
    if (!e) return;
    int dur = (int)video_get_duration();
    if (dur <= 0) return;
    int pct = lv_slider_get_value(ui_timeSlider);
    int target = dur * pct / 100;
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_VALUE_CHANGED) {
        g_vdragging = 60;
        char b[16];
        snprintf(b, sizeof(b), "%d:%02d", target / 60, target % 60);
        lv_label_set_text(ui_nowtimeLabel, b);
    } else if (c == LV_EVENT_RELEASED) {
        video_seek(target);
        g_vdragging = 15;
    }
}
