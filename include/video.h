#ifndef VIDEO_H
#define VIDEO_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl/lvgl.h"
#include <stdint.h>

#define VIDEO_MAX_PLAYLIST 64

#define VIDEO_STATE_STOPPED  0
#define VIDEO_STATE_PLAYING  1
#define VIDEO_STATE_PAUSED   2

// ── Low-level player API (pl_mpeg wrapper) ──────────────
void   video_play(const char *path);
void   video_stop(void);
void   video_pause(void);
void   video_resume(void);
int    video_is_playing(void);
int    video_is_paused(void);
double video_get_time(void);
double video_get_duration(void);
void   video_seek(double sec);
void   video_set_volume(int pct);
int    video_get_frame(uint8_t **buf, int *w, int *h);

// ── High-level UI module API ────────────────────────────
void video_module_init(void);
void video_module_deinit(void);

void video_set_playlist(const char *paths[], int count);
int  video_play_index(int index);
void video_stop_playback(void);
void video_toggle_pause(void);
void video_next(void);
void video_prev(void);
void video_set_volume_pct(int pct);

int video_get_state(void);
int video_get_current_index(void);
int video_get_volume(void);

// Called each frame from LVGL timer to copy decoded frame to display + update UI
void video_render_frame(void);

// UI event handlers — called from ui_events.c
void video_on_play_pause_btn(lv_event_t *e);
void video_on_next_btn(lv_event_t *e);
void video_on_prev_btn(lv_event_t *e);
void video_on_volume_slider(lv_event_t *e);
void video_on_time_slider(lv_event_t *e);

#ifdef __cplusplus
}
#endif

#endif
