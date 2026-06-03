// video.c —— pl_mpeg 解码 + ring buffer 音频 + LVGL 渲染 + UI 控制
//
// 架构说明：
//   解码线程 → on_audio() 非阻塞 push → ring buffer → 音频线程 阻塞 write OSS
//   解码线程 → on_video() 写 g_frame → UI 定时器 取帧 swizzle → LVGL image
//   OSS write 不再阻塞解码循环，确保视频帧率不受音频 I/O 影响。
//
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

// ── Forward declarations ─────────────────────────────────
void audio_stop(void);

// ═══════════════════════════════════════════════════════════
//  Ring buffer (SPSC, lock-free)
// ═══════════════════════════════════════════════════════════
#define RING_MASK  0x7FFF          // 32768 个采样点 ≈ 0.74s @ 44100Hz
#define RING_SIZE  (RING_MASK + 1)

static short          g_ring[RING_SIZE];
static volatile int   g_ring_w = 0;   // decode 线程写
static volatile int   g_ring_r = 0;   // audio 线程读
static pthread_t      g_audio_tid;
static volatile int   g_audio_alive = 0;
static volatile int   g_audio_stop  = 0;
static int            g_audio_rate  = 44100;

static int ring_available(void)
{
    return (g_ring_w - g_ring_r) & RING_MASK;
}

static int ring_space(void)
{
    return RING_SIZE - 1 - ring_available();
}

// push n 个 short → ring；放不下则丢弃（不阻塞）
static void ring_push(const short *data, int n)
{
    for (int i = 0; i < n; i++) {
        int next = (g_ring_w + 1) & RING_MASK;
        if (next == g_ring_r) return;   // 满，丢弃剩余采样
        g_ring[g_ring_w] = data[i];
        g_ring_w = next;
    }
}

// pop 最多 max 个 short → buf；返回实际读取数
static int ring_pop(short *buf, int max)
{
    int n = 0;
    while (n < max && g_ring_r != g_ring_w) {
        buf[n++] = g_ring[g_ring_r];
        g_ring_r = (g_ring_r + 1) & RING_MASK;
    }
    return n;
}

// ── 音频输出线程 ─────────────────────────────────────────
static void *audio_thread(void *arg)
{
    (void)arg;

    // 打开 DSP
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
            write(dsp, buf, n * sizeof(short));   // 这里阻塞不会影响解码线程
        } else {
            usleep(5000);   // 缓冲区空，短暂休眠
        }
    }

    // 排空残余数据
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
    g_ring_w      = 0;
    g_ring_r      = 0;
    g_audio_stop  = 0;
    g_audio_alive = 1;
    pthread_create(&g_audio_tid, NULL, audio_thread, NULL);
    pthread_detach(g_audio_tid);
}

static void audio_thread_stop(void)
{
    if (!g_audio_alive) return;
    g_audio_stop = 1;
    for (int i = 0; i < 40 && g_audio_alive; i++)
        usleep(50000);   // 等待最多 2 秒
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

static volatile int g_width  = 0;
static volatile int g_height = 0;
static volatile int g_frame_ready = 0;

static pthread_t       g_thread;
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint8_t        *g_frame = NULL;

// ── 音频回调 (解码线程内调用，快速非阻塞) ─────────────────
static void on_audio(plm_t *pl, plm_samples_t *s, void *user)
{
    (void)pl;
    (void)user;
    if (!s || s->count <= 0) return;

    float gain = (float)g_low_volume / 100.f;
    int   n    = s->count;

    // 栈上 buffer，避免每次 malloc/free
    short buf[1024];
    int out = 0;
    int max = (int)(sizeof(buf) / sizeof(buf[0]));

    for (int i = 0; i < n * 2 && out < max; i += 2) {
        float f = s->interleaved[i] * gain;
        if (f >  1.0f) f =  1.0f;
        if (f < -1.0f) f = -1.0f;
        buf[out++] = (short)(f * 32767.f) ^ 0x8000;
        buf[out++] = (short)(f * 32767.f) ^ 0x8000;
    }
    ring_push(buf, out);   // 非阻塞，满了就丢
}

// ── 视频回调 (解码线程内调用，只拷贝帧数据) ──────────────
static void on_video(plm_t *pl, plm_frame_t *f, void *user)
{
    (void)pl;
    (void)user;
    if (!f || !g_frame) return;

    pthread_mutex_lock(&g_mutex);
    plm_frame_to_rgba(f, g_frame, g_width * 4);
    g_frame_ready = 1;
    pthread_mutex_unlock(&g_mutex);
    g_current_time = f->time;
}

// ── 解码线程 ─────────────────────────────────────────────
static void *th(void *arg)
{
    const char *path = (const char *)arg;
    plm_t *pl = plm_create_with_filename(path);
    if (!pl) { free(arg); return NULL; }

    plm_set_loop(pl, 0);
    g_width       = plm_get_width(pl);
    g_height      = plm_get_height(pl);
    g_duration    = plm_get_duration(pl);
    g_audio_rate  = plm_get_samplerate(pl);

    pthread_mutex_lock(&g_mutex);
    g_frame = malloc(g_width * g_height * 4);
    pthread_mutex_unlock(&g_mutex);

    plm_set_video_decode_callback(pl, on_video, NULL);
    plm_set_audio_decode_callback(pl, on_audio, NULL);

    // 启动音频输出线程（首次音频回调到来前启动，保证有数据可写）
    audio_thread_start();

    double t = 0;
    int seek_skip = 0;
    while (g_playing && !g_stop) {
        while (g_paused && g_playing) usleep(100000);

        if (g_seek_target >= 0) {
            plm_seek(pl, g_seek_target, 1);
            t = g_seek_target;
            g_current_time = g_seek_target;
            g_seek_target = -1;
            seek_skip = 2;
            continue;
        }
        if (seek_skip > 0) { seek_skip--; continue; }

        t += 0.03;
        plm_decode(pl, t);
        if (plm_has_ended(pl)) break;
    }

    // 停止音频线程（排空 ring buffer 残余）
    audio_thread_stop();
    plm_destroy(pl);
    free(arg);
    g_alive   = 0;
    g_playing = 0;
    return NULL;
}

// ── Low-level public API ─────────────────────────────────

void video_play(const char *path)
{
    video_stop();
    audio_stop();

    char *copy = strdup(path);
    g_playing       = 1;
    g_paused        = 0;
    g_stop          = 0;
    g_seek_target   = -1;
    g_current_time  = 0;
    g_alive         = 1;
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

void video_pause(void)          { g_paused = 1; }
void video_resume(void)         { g_paused = 0; }
int  video_is_playing(void)     { return g_playing && !g_paused; }
int  video_is_paused(void)      { return g_paused; }
double video_get_time(void)     { return g_current_time; }
double video_get_duration(void) { return g_duration; }
void video_seek(double seconds) { g_seek_target = seconds; }

void video_set_volume(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    g_low_volume = pct;
}

int video_get_frame(uint8_t **buf, int *w, int *h)
{
    if (!g_frame_ready || !g_frame) return 0;
    *w   = g_width;
    *h   = g_height;
    *buf = g_frame;
    return 1;
}

// ═══════════════════════════════════════════════════════════
//  高层 UI 模块 (playlist / 状态 / 事件 / LVGL 渲染)
// ═══════════════════════════════════════════════════════════

#define DEFAULT_VPLS_COUNT 4
static const char *g_default_vpls[DEFAULT_VPLS_COUNT] = {
    "/Mydata/video/1.mpg", "/Mydata/video/2.mpg",
    "/Mydata/video/3.mpg", "/Mydata/video/4.mpg"
};
static const char **g_vpls      = g_default_vpls;
static int          g_vpls_cnt  = DEFAULT_VPLS_COUNT;
static int          g_vcur      = 0;

static int          g_state     = VIDEO_STATE_STOPPED;
static int          g_vol       = 100;
static int          g_dragging  = 0;
static lv_timer_t  *g_ui_timer = NULL;

// ── Video render state ───────────────────────────────────
static lv_obj_t    *g_render_img  = NULL;
static uint8_t     *g_sw_buf      = NULL;   // RGBA→BGRA swizzle buffer
static lv_img_dsc_t g_render_desc;
static int          g_alloc_w     = 0;
static int          g_alloc_h     = 0;

static void fmt_time_v(int ms, char *buf, int sz)
{
    int m = ms / 60000, s = (ms % 60000) / 1000;
    snprintf(buf, sz, "%d:%02d", m, s);
}

static void ensure_render_buf(int w, int h)
{
    if (g_sw_buf && g_alloc_w == w && g_alloc_h == h) return;
    free(g_sw_buf);
    g_sw_buf = malloc(w * h * 4);
    g_render_desc.header.cf        = LV_IMG_CF_TRUE_COLOR_ALPHA;
    g_render_desc.header.w         = w;
    g_render_desc.header.h         = h;
    g_render_desc.data_size        = w * h * 4;
    g_render_desc.data             = g_sw_buf;
    g_alloc_w = w;
    g_alloc_h = h;

    // 等比缩放适配容器
    lv_coord_t cw = lv_obj_get_width(ui_videoContain);
    lv_coord_t ch = lv_obj_get_height(ui_videoContain);
    float scale = ((float)w / cw > (float)h / ch) ? (float)cw / w : (float)ch / h;
    lv_obj_set_size(g_render_img, (lv_coord_t)(w * scale), (lv_coord_t)(h * scale));
    lv_obj_center(g_render_img);
}

// ── UI 定时器：渲染帧 + 更新进度条 ──────────────────────
static void ui_timer_cb(lv_timer_t *t)
{
    (void)t;

    // 播放自然结束 → 停止
    if (g_state == VIDEO_STATE_PLAYING && !g_alive && !video_is_playing()) {
        video_stop_playback();
        return;
    }

    // 取出解码帧并渲染到 ui_videoContain
    uint8_t *src;
    int w, h;
    if (video_get_frame(&src, &w, &h)) {
        ensure_render_buf(w, h);
        int px = w * h;
        for (int i = 0; i < px; i++) {
            g_sw_buf[i * 4 + 0] = src[i * 4 + 2];   // R→B
            g_sw_buf[i * 4 + 1] = src[i * 4 + 1];   // G→G
            g_sw_buf[i * 4 + 2] = src[i * 4 + 0];   // B→R
            g_sw_buf[i * 4 + 3] = src[i * 4 + 3];   // A→A
        }
        lv_img_set_src(g_render_img, &g_render_desc);
    }

    if (g_state == VIDEO_STATE_STOPPED) return;

    int cur_ms = (int)(video_get_time() * 1000);
    int dur_ms = (int)(video_get_duration() * 1000);
    if (dur_ms > 0) {
        if (g_dragging > 0) g_dragging--;
        if (!g_dragging)
            lv_slider_set_value(ui_timeSlider, cur_ms * 100 / dur_ms, LV_ANIM_OFF);
    }
    char buf[16];
    fmt_time_v(cur_ms, buf, sizeof(buf));
    lv_label_set_text(ui_nowtimeLabel, buf);
    if (dur_ms > 0) {
        fmt_time_v(dur_ms, buf, sizeof(buf));
        lv_label_set_text(ui_sumtimeLabel, buf);
    }
}

// ── 启动播放播放列表项 ─────────────────────────────────
static void start_playback(int idx)
{
    if (idx < 0 || idx >= g_vpls_cnt) return;
    g_vcur  = idx;
    g_state = VIDEO_STATE_PLAYING;

    lv_slider_set_value(ui_timeSlider, 0, LV_ANIM_OFF);
    lv_label_set_text(ui_nowtimeLabel, "0:00");
    lv_label_set_text(ui_sumtimeLabel, "0:00");
    g_dragging = 0;

    if (g_render_img) lv_obj_add_flag(g_render_img, LV_OBJ_FLAG_HIDDEN);

    const char *name = strrchr(g_vpls[idx], '/');
    lv_label_set_text(ui_videoText, name ? name + 1 : g_vpls[idx]);

    video_play(g_vpls[idx]);

    if (g_render_img) lv_obj_clear_flag(g_render_img, LV_OBJ_FLAG_HIDDEN);
}

// ── High-level public API ────────────────────────────────

void video_module_init(void)
{
    g_state    = VIDEO_STATE_STOPPED;
    g_vcur     = 0;
    g_vol      = 100;
    g_dragging = 0;
    video_set_volume(g_vol);

    if (!g_render_img) {
        // 在 ui_videoContain 内创建渲染用的 image 对象
        g_render_img = lv_img_create(ui_videoContain);
        lv_obj_center(g_render_img);
        lv_obj_add_flag(g_render_img, LV_OBJ_FLAG_HIDDEN);
    }

    lv_slider_set_value(ui_Slider4, g_vol, LV_ANIM_OFF);

    if (!g_ui_timer)
        g_ui_timer = lv_timer_create(ui_timer_cb, 33, NULL);
}

void video_module_deinit(void)
{
    video_stop_playback();
    if (g_ui_timer) { lv_timer_del(g_ui_timer); g_ui_timer = NULL; }
    if (g_render_img) { lv_obj_del(g_render_img); g_render_img = NULL; }
    free(g_sw_buf);
    g_sw_buf  = NULL;
    g_alloc_w = 0;
    g_alloc_h = 0;
}

void video_set_playlist(const char *paths[], int count)
{
    if (!paths || count <= 0 || count > VIDEO_MAX_PLAYLIST) return;
    g_vpls     = paths;
    g_vpls_cnt = count;
    g_vcur     = 0;
}

int video_play_index(int index)
{
    if (index < 0 || index >= g_vpls_cnt) return -1;
    start_playback(index);
    return 0;
}

void video_stop_playback(void)
{
    video_stop();
    g_state = VIDEO_STATE_STOPPED;
    lv_slider_set_value(ui_timeSlider, 0, LV_ANIM_OFF);
    lv_label_set_text(ui_nowtimeLabel, "0:00");
    lv_label_set_text(ui_sumtimeLabel, "0:00");
    if (g_render_img) lv_obj_add_flag(g_render_img, LV_OBJ_FLAG_HIDDEN);
}

void video_toggle_pause(void)
{
    if (g_state == VIDEO_STATE_PLAYING)       { video_pause();  g_state = VIDEO_STATE_PAUSED; }
    else if (g_state == VIDEO_STATE_PAUSED)   { video_resume(); g_state = VIDEO_STATE_PLAYING; }
    else if (g_state == VIDEO_STATE_STOPPED)  { start_playback(g_vcur); }
}

void video_next(void)
{
    if (g_vpls_cnt == 0) return;
    g_vcur = (g_vcur + 1) % g_vpls_cnt;
    start_playback(g_vcur);
}

void video_prev(void)
{
    if (g_vpls_cnt == 0) return;
    g_vcur = (g_vcur - 1 + g_vpls_cnt) % g_vpls_cnt;
    start_playback(g_vcur);
}

void video_set_volume_pct(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    g_vol = pct;
    video_set_volume(pct);
}

int video_get_state(void)          { return g_state; }
int video_get_current_index(void)  { return g_vcur; }
int video_get_volume(void)         { return g_vol; }

void video_render_frame(void)
{
    uint8_t *src;
    int w, h;
    if (!video_get_frame(&src, &w, &h)) return;
    ensure_render_buf(w, h);
    int px = w * h;
    for (int i = 0; i < px; i++) {
        g_sw_buf[i * 4 + 0] = src[i * 4 + 2];
        g_sw_buf[i * 4 + 1] = src[i * 4 + 1];
        g_sw_buf[i * 4 + 2] = src[i * 4 + 0];
        g_sw_buf[i * 4 + 3] = src[i * 4 + 3];
    }
    lv_img_set_src(g_render_img, &g_render_desc);
}

// ── UI event handlers ────────────────────────────────────

void video_on_play_pause_btn(lv_event_t *e) { (void)e; video_toggle_pause(); }
void video_on_next_btn(lv_event_t *e)       { (void)e; video_next(); }
void video_on_prev_btn(lv_event_t *e)       { (void)e; video_prev(); }

void video_on_volume_slider(lv_event_t *e)
{
    int vol = lv_slider_get_value(lv_event_get_target(e));
    video_set_volume_pct(vol);
}

void video_on_time_slider(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    double dur = video_get_duration();

    if (code == LV_EVENT_PRESSED) {
        g_dragging = 3;
    } else if (code == LV_EVENT_VALUE_CHANGED && g_dragging && dur > 0) {
        int pct = lv_slider_get_value(lv_event_get_target(e));
        int ms  = (int)(dur * 1000 * pct / 100);
        char buf[16];
        fmt_time_v(ms, buf, sizeof(buf));
        lv_label_set_text(ui_nowtimeLabel, buf);
    } else if (code == LV_EVENT_RELEASED && dur > 0) {
        int pct = lv_slider_get_value(lv_event_get_target(e));
        video_seek(dur * pct / 100);
        g_dragging = 0;
    }
}
