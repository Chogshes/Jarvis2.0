#ifndef PIPELINE_H
#define PIPELINE_H

#include "lvgl/lvgl.h"
#include "deepseek.h"

// ── Pipeline globals ─────────────────────────────────────
extern lv_timer_t *g_pipeline_timer;
extern lv_timer_t *g_control_timer;
extern int g_has_intent;
extern int g_step;
extern int g_tick;
extern char g_asr_text[512];
extern char g_ai_reply[2048];

// ── Callbacks / timer functions ───────────────────────────
void on_asr_result(const char *text);
void on_deepseek_result(const Intent *intent, const char *reply);
void process_pipeline(lv_timer_t *t);

#endif
