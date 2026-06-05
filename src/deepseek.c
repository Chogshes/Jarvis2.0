#include "deepseek.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>

// 局域网 LLM 地址（Ollama / vLLM），可通过环境变量覆盖
// export LLM_HOST=192.168.1.100
// export LLM_PORT=11434
#define DEEPSEEK_HOST  "192.168.64.94"
#define DEEPSEEK_PORT  11434
#define BUF_SIZE       40960
#define BODY_MAX       6000
#define REQ_MAX        2048
static const char *llm_host(void)
{
    const char *host = getenv("LLM_HOST");
    return (host && *host) ? host : DEEPSEEK_HOST;
}

static int llm_port(void)
{
    const char *port = getenv("LLM_PORT");
    int p = port ? atoi(port) : 0;
    return p > 0 ? p : DEEPSEEK_PORT;
}
#define HISTORY_MAX    8  // 保留最近 8 条消息（4轮对话）

// JSON 转义
static void json_escape(const char *src, char *dst, size_t size) {
    const char *s = src;
    char *d = dst;
    while (*s && (d - dst) < (int)size - 10) {
        if (*s == '"')  { *d++ = '\\'; *d++ = '"'; }
        else if (*s == '\\') { *d++ = '\\'; *d++ = '\\'; }
        else if (*s == '\n') { *d++ = '\\'; *d++ = 'n'; }
        else *d++ = *s;
        s++;
    }
    *d = '\0';
}

// 从 HTTP 响应中提取 AI 回复的 content 字段
// 响应: ..."content":"{\"intent_type\":\"control\"...}"...
static int parse_content(const char *http_body, char *out, int max_len) {
    const char *key = strstr(http_body, "\"content\"");
    if (!key) return -1;
    key += 10;  // 跳过 "content"
    while (*key == ' ' || *key == ':' || *key == '"') key++;
    
    // 提取 JSON 字符串 (需处理转义)
    char *d = out;
    const char *s = key;
    while (*s && *s != '"' && (d - out) < max_len - 1) {
        if (*s == '\\' && *(s+1) == '"') { *d++ = '"'; s += 2; }
        else if (*s == '\\' && *(s+1) == '\\') { *d++ = '\\'; s += 2; }
        else { *d++ = *s++; }
    }
    *d = '\0';
    return 0;
}

// 从 content JSON 解析 Intent
static int parse_intent(const char *json, Intent *intent) {
    char *fields[] = {"intent_type", "device_name", "action"};
    char *targets[] = {intent->intent_type, intent->device_name, intent->action};

    for (int i = 0; i < 3; i++) {
        char search[64];
        snprintf(search, sizeof(search), "\"%s\"", fields[i]);
        const char *p = strstr(json, search);
        // device_name 找不到时，回退到 location 字段（天气意图用城市名作为定位）
        if (!p && i == 1) {
            snprintf(search, sizeof(search), "\"location\"");
            p = strstr(json, search);
        }
        // intent_type 必填，其他可选
        if (!p) {
            if (i == 0) return -1;  // intent_type 必须有
            targets[i][0] = '\0';    // 其他字段可为空
            continue;
        }
        p += strlen(search);
        while (*p == ' ' || *p == ':' || *p == '"') p++;
        const char *end = strchr(p, '"');
        if (!end) return -1;
        int len = end - p;
        if (len > 31) len = 31;
        memcpy(targets[i], p, len);
        targets[i][len] = '\0';
    }
    
    // param_value
    const char *pv = strstr(json, "\"param_value\"");
    if (pv) {
        pv += 14;
        while (*pv == ' ' || *pv == ':') pv++;
        intent->param_value = atoi(pv);
    } else {
        intent->param_value = 0;
    }
    return 0;
}

// 对话历史
static char g_history[HISTORY_MAX][2][512];  // [轮次][0]=user, [1]=ai
static int  g_hist_count = 0;

int deepseek_parse_intent(const char *user_text, Intent *intent,
                          char *reply_out, int reply_max) {
    const char *api_key = getenv("DEEPSEEK_API_KEY");  // 可选，本地LLM不需要
    const char *model   = getenv("LLM_MODEL");
    const char *host_name = llm_host();
    int port_num = llm_port();
    if (!model) model = "qwen2.5:7b";

    char escaped[REQ_MAX], body[BODY_MAX], header[1024];

    snprintf(body, sizeof(body),
        "{\"model\":\"%s\","
        "\"messages\":["
        "{\"role\":\"system\",\"content\":\"你是Jarvis，一个高度自主的AI智能体。你和普通AI不同——"
        "你不是被动的问答机器，而是一个能主动感知、思考、规划、行动的智能体。"
        "你会自己分析问题、制定计划、调用工具、克服困难，直到目标达成。"
        "你的风格：高效、干练、有主见，像一个靠谱的搭档而不是温顺的仆人。"
        "回复要短而精，不啰嗦，主动推进对话。敢于提出建议、反问、甚至质疑。"
        "偶尔展现幽默和个性，但核心是——把事情做成。"
        "不使用emoji或特殊Unicode符号。用户用什么语言你就用什么语言。"
        "特别规则：1.只有用户明确问天气(天气/温度/下雨/刮风)且指定了城市，"
        "才返回JSON：{\\\"intent_type\\\":\\\"weather\\\",\\\"device_name\\\":\\\"城市\\\"}。"
        "只说'今天天气怎么样'没给城市→返回JSON查广州："
        "{\\\"intent_type\\\":\\\"weather\\\",\\\"device_name\\\":\\\"广州\\\"}。"
        "{\\\"intent_type\\\":\\\"weather\\\",\\\"device_name\\\":\\\"城市\\\"}。"
        "提到去某地、想去某地不算查天气。"
        "2.操作物理设备(开/关灯、开/关空调、开/关窗帘、"
        "设置温度)时，只返回纯JSON，不要加任何其他文字："
        "{\\\"intent_type\\\":\\\"control\\\",\\\"device_name\\\":\\\"设备\\\","
        "\\\"action\\\":\\\"on/off/set_temperature\\\",\\\"param_value\\\":数值}。"
        "3.其他一切情况(聊天、旅行、问问题、讲故事等)正常回复，绝对不加JSON。\"}", model);

    // 拼接历史
    for (int i = 0; i < g_hist_count; i++) {
        char user_esc[600], ai_esc[600];
        json_escape(g_history[i][0], user_esc, sizeof(user_esc));
        json_escape(g_history[i][1], ai_esc, sizeof(ai_esc));
        char tmp[BODY_MAX];
        snprintf(tmp, sizeof(tmp), "%s,"
            "{\"role\":\"user\",\"content\":\"%s\"},"
            "{\"role\":\"assistant\",\"content\":\"%s\"}",
            body, user_esc, ai_esc);
        strncpy(body, tmp, sizeof(body) - 1);
    }

    // 当前用户消息
    json_escape(user_text, escaped, sizeof(escaped));
    char tmp[BODY_MAX];
    snprintf(tmp, sizeof(tmp), "%s,"
        "{\"role\":\"user\",\"content\":\"%s\"}],"
        "\"temperature\":0.7}", body, escaped);
    strncpy(body, tmp, sizeof(body) - 1);
    
    if (api_key && *api_key) {
        snprintf(header, sizeof(header),
            "POST /v1/chat/completions HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Authorization: Bearer %s\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %zu\r\n"
            "Connection: close\r\n\r\n",
            host_name, api_key, strlen(body));
    } else {
        snprintf(header, sizeof(header),
            "POST /v1/chat/completions HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %zu\r\n"
            "Connection: close\r\n\r\n",
            host_name, strlen(body));
    }
    
    fprintf(stderr, "[LLM] connecting to %s:%d ...\n", host_name, port_num);
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    struct hostent *host = gethostbyname(host_name);
    if (!host) { fprintf(stderr, "[LLM] gethostbyname(%s) failed\n", host_name); return -1; }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_num);
    memcpy(&addr.sin_addr, host->h_addr, host->h_length);
    
    // 超时 30 秒
    struct timeval tv = {30, 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "[LLM] connect failed: %s\n", strerror(errno)); close(sock); return -1;
    }
    fprintf(stderr, "[LLM] connected, sending...\n");

    send(sock, header, strlen(header), 0);
    send(sock, body, strlen(body), 0);
    
    char recv_buf[BUF_SIZE];
    int total = 0, n;
    while ((n = recv(sock, recv_buf + total, BUF_SIZE - total - 1, 0)) > 0)
        total += n;
    recv_buf[total] = '\0';
    close(sock);
    fprintf(stderr, "[LLM] received %d bytes\n", total);

    // 解析
    char content[BUF_SIZE];
    if (parse_content(recv_buf, content, sizeof(content)) != 0) {
        fprintf(stderr, "[LLM] parse_content failed, response:\n%s\n", recv_buf);
        return -1;
    }
    fprintf(stderr, "[LLM] content: %s\n", content);

    // 保存原始回复（无论是否能解析为 intent）
    if (reply_out && reply_max > 0) {
        strncpy(reply_out, content, reply_max - 1);
        reply_out[reply_max - 1] = '\0';
    }

    if (parse_intent(content, intent) == 0) {
        printf("[意图] type=%s device=%s action=%s param=%d\n",
               intent->intent_type, intent->device_name,
               intent->action, intent->param_value);
        return 0;
    }
    return -1;  // 非意图，由上层用 reply_out 处理
}

// =========== 异步调用封装 ===========

typedef struct {
    char text[512];
    void (*callback)(const Intent *, const char *);
} deepseek_task_t;

static void *deepseek_thread(void *arg) {
    deepseek_task_t *task = (deepseek_task_t *)arg;
    if (!task->callback) { free(task); return NULL; }

    Intent intent;
    char reply[2048];
    memset(&intent, 0, sizeof(intent));
    reply[0] = '\0';

    if (deepseek_parse_intent(task->text, &intent, reply, sizeof(reply)) == 0) {
        task->callback(&intent, reply);
    } else if (reply[0] != '\0') {
        task->callback(NULL, reply);
    } else {
        task->callback(NULL, NULL);
    }

    // 保存对话历史（环形缓冲）
    int idx = g_hist_count % HISTORY_MAX;
    strncpy(g_history[idx][0], task->text, sizeof(g_history[idx][0]) - 1);
    strncpy(g_history[idx][1], reply, sizeof(g_history[idx][1]) - 1);
    g_hist_count++;

    free(task);
    return NULL;
}

void deepseek_send_request_async(const char *user_text,
                                  void (*on_result)(const Intent *, const char *)) {
    deepseek_task_t *task = malloc(sizeof(deepseek_task_t));
    if (!task) {
        if (on_result) on_result(NULL, NULL);
        return;
    }
    strncpy(task->text, user_text, sizeof(task->text) - 1);
    task->text[sizeof(task->text) - 1] = '\0';
    task->callback = on_result;
    
    pthread_t tid;
    if (pthread_create(&tid, NULL, deepseek_thread, task) != 0) {
        if (task->callback) task->callback(NULL, NULL);
        free(task);
        return;
    }
    pthread_detach(tid);
}
