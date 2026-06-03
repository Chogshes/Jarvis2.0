#ifndef MUSIC_H
#define MUSIC_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl/lvgl.h"

#define MUSIC_MAX_PLAYLIST 64

#define MUSIC_STATE_STOPPED  0
#define MUSIC_STATE_PLAYING  1
#define MUSIC_STATE_PAUSED   2

void music_init(void);
void music_deinit(void);

void music_set_playlist(const char *paths[], int count);
int  music_play(int index);
void music_stop(void);
void music_pause(void);
void music_resume(void);
void music_toggle_pause(void);
void music_next(void);
void music_prev(void);
void music_set_volume(int pct);

int music_get_state(void);
int music_get_current_index(void);
int music_get_volume(void);
int music_get_playlist_count(void);

// UI event handlers — called from ui_events.c
void music_on_play_pause_btn(lv_event_t *e);
void music_on_next_btn(lv_event_t *e);
void music_on_prev_btn(lv_event_t *e);
void music_on_volume_slider(lv_event_t *e);
void music_on_time_slider(lv_event_t *e);

#ifdef __cplusplus
}
#endif

#endif
