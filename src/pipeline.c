// pipeline.c —— ASR → DeepSeek → TTS → 控制 管道
#include "pipeline.h"

lv_timer_t *g_pipeline_timer = NULL;
lv_timer_t *g_control_timer = NULL;
int g_has_intent = 0;
int g_step = 1;
int g_tick = 0;
char g_asr_text[512] = {0};
char g_ai_reply[2048] = {0};

// ASR 识别结果回调
void on_asr_result(const char *text)
{
    // TODO: 收到 ASR 文本后填入输入框并触发 send_cmd_fun
    (void)text;
}

void on_deepseek_result(const Intent *intent, const char *reply)
{
    // TODO: 处理 DeepSeek 返回结果（控制设备 / TTS 朗读）
    (void)intent;
    (void)reply;
}

void process_pipeline(lv_timer_t *t)
{
    // TODO: 分步处理 pipeline（step 1: ASR → step 2: LLM → step 3: TTS）
    (void)t;
}
