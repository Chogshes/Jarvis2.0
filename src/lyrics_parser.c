#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define MAX_LYRICS_LINE 100
#define MAX_LINE_LENGTH 256

typedef struct {
    int time_ms;           // 时间戳（毫秒）
    char text[MAX_LINE_LENGTH]; // 歌词内容
} LyricLine;

static LyricLine g_lyrics[MAX_LYRICS_LINE];
static int g_lyrics_count = 0;
static int g_current_line = 0;
static char g_current_song[256] = {0};

/**
 * @brief 解析时间戳字符串 "mm:ss.ms" 为毫秒
 */
static int parse_time(const char *time_str) {
    int minutes = 0, seconds = 0, ms = 0;
    if (sscanf(time_str, "%d:%d.%d", &minutes, &seconds, &ms) != 3) {
        sscanf(time_str, "%d:%d", &minutes, &seconds);
    }
    return minutes * 60 * 1000 + seconds * 1000 + ms;
}

/**
 * @brief 加载并解析 LRC 歌词文件
 * @param mp3_path MP3文件路径，自动查找同名.lrc文件
 * @return 成功返回0，失败返回-1
 */
int lyrics_load_from_mp3(const char *mp3_path) {
    if (!mp3_path) return -1;
    
    // 检查是否是同一首歌，避免重复加载
    if (strcmp(g_current_song, mp3_path) == 0 && g_lyrics_count > 0) {
        return 0;
    }
    
    // 清空之前的歌词
    g_lyrics_count = 0;
    g_current_line = 0;
    strncpy(g_current_song, mp3_path, sizeof(g_current_song)-1);
    
    // 构建歌词文件路径（将.mp3替换为.lrc）
    char lrc_path[256];
    strncpy(lrc_path, mp3_path, sizeof(lrc_path)-1);
    char *dot = strrchr(lrc_path, '.');
    if (dot) {
        strcpy(dot, ".lrc");
    } else {
        strcat(lrc_path, ".lrc");
    }
    
    FILE *fp = fopen(lrc_path, "r");
    if (!fp) {
        printf("歌词文件不存在: %s\n", lrc_path);
        return -1;
    }
    
    char line[MAX_LINE_LENGTH];
    while (fgets(line, sizeof(line), fp) && g_lyrics_count < MAX_LYRICS_LINE) {
        char *start = strchr(line, '[');
        char *end = strchr(line, ']');
        
        if (start && end && start < end) {
            *end = '\0';
            int time_ms = parse_time(start + 1);
            
            char *text = end + 1;
            text[strcspn(text, "\n\r")] = '\0';
            
            // 跳过空行、标题行、词曲信息行
            int skip = (strlen(text) == 0 || strstr(text, "-") != NULL);
            // 词： 曲： 编曲： 制作：开头的跳过（按字节检查常见前缀）
            if (!skip && (text[0] == '\xe8' && text[1] == '\xaf' && text[2] == '\x8d')) skip = 1; // 词
            if (!skip && (text[0] == '\xe6' && text[1] == '\x9b' && text[2] == '\xb2')) skip = 1; // 曲
            if (!skip && (text[0] == '\xe7' && text[1] == '\xbc' && text[2] == '\x96')) skip = 1; // 编
            if (!skip && (text[0] == '\xe5' && text[1] == '\x88' && text[2] == '\xb6')) skip = 1; // 制
            if (!skip && strcmp(text, "end") == 0) skip = 1;  // 结束符
            if (!skip) {
                g_lyrics[g_lyrics_count].time_ms = time_ms;
                strncpy(g_lyrics[g_lyrics_count].text, text, sizeof(g_lyrics[g_lyrics_count].text)-1);
                g_lyrics_count++;
            }
        }
    }
    
    fclose(fp);
    printf("加载歌词: %s, 共 %d 行\n", lrc_path, g_lyrics_count);
    return 0;
}

/**
 * @brief 根据当前播放时间获取歌词
 * @param current_time_ms 当前播放时间（毫秒）
 * @return 当前应显示的歌词
 */
const char *lyrics_get_current(int current_time_ms) {
    if (g_lyrics_count == 0) return "";
    
    for (int i = g_lyrics_count - 1; i >= 0; i--) {
        if (g_lyrics[i].time_ms <= current_time_ms) {
            if (g_current_line != i) {
                g_current_line = i;
            }
            return g_lyrics[i].text;
        }
    }
    return g_lyrics[0].text;
}

/**
 * @brief 获取歌词总行数
 */
int lyrics_get_total_count(void) {
    return g_lyrics_count;
}

/**
 * @brief 重置歌词位置
 */
void lyrics_reset(void) {
    g_current_line = 0;
}

/**
 * @brief 检查是否有歌词
 */
int lyrics_has_loaded(void) {
    return g_lyrics_count > 0;
}

/**
 * @brief 获取全部歌词（用于完整显示）
 * @param buffer 输出缓冲区
 * @param max_len 缓冲区最大长度
 * @return 实际写入的字符数
 */
int lyrics_get_all(char *buffer, int max_len) {
    if (g_lyrics_count == 0) {
        buffer[0] = '\0';
        return 0;
    }
    
    buffer[0] = '\0';
    int pos = 0;
    
    for (int i = 0; i < g_lyrics_count; i++) {
        // 格式：[时间] 歌词内容
        int time_min = g_lyrics[i].time_ms / 60000;
        int time_sec = (g_lyrics[i].time_ms % 60000) / 1000;
        int time_ms = g_lyrics[i].time_ms % 1000;
        
        int written = snprintf(buffer + pos, max_len - pos - 1, "[%02d:%02d.%03d] %s\n", 
                               time_min, time_sec, time_ms, g_lyrics[i].text);
        
        if (written <= 0 || pos + written >= max_len - 1) {
            break;
        }
        pos += written;
    }
    
    buffer[pos] = '\0';
    return pos;
}

/**
 * @brief 获取指定行的歌词
 * @param index 行索引（从0开始）
 * @return 歌词内容，如果索引无效返回NULL
 */
const char *lyrics_get_line(int index) {
    if (index < 0 || index >= g_lyrics_count) {
        return NULL;
    }
    return g_lyrics[index].text;
}

/**
 * @brief 获取当前播放时间对应的歌词行索引
 * @param current_time_ms 当前播放时间（毫秒）
 * @return 当前行索引
 */
int lyrics_get_current_line_index(int current_time_ms) {
    if (g_lyrics_count == 0) return -1;
    
    for (int i = g_lyrics_count - 1; i >= 0; i--) {
        if (g_lyrics[i].time_ms <= current_time_ms) {
            g_current_line = i;
            return i;
        }
    }
    return 0;
}