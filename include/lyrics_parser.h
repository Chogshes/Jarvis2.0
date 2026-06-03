#ifndef LYRICS_PARSER_H
#define LYRICS_PARSER_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 从MP3路径加载歌词（自动查找同名.lrc文件）
 * @param mp3_path MP3文件路径
 * @return 成功返回0，失败返回-1
 */
int lyrics_load_from_mp3(const char *mp3_path);

/**
 * @brief 根据当前播放时间获取歌词
 * @param current_time_ms 当前播放时间（毫秒）
 * @return 当前应显示的歌词
 */
const char *lyrics_get_current(int current_time_ms);

/**
 * @brief 获取歌词总行数
 */
int lyrics_get_total_count(void);

/**
 * @brief 重置歌词播放位置
 */
void lyrics_reset(void);

/**
 * @brief 检查是否已加载歌词
 */
int lyrics_has_loaded(void);

/**
 * @brief 获取全部歌词（用于完整显示）
 * @param buffer 输出缓冲区
 * @param max_len 缓冲区最大长度
 * @return 实际写入的字符数
 */
int lyrics_get_all(char *buffer, int max_len);

/**
 * @brief 获取指定行的歌词
 * @param index 行索引（从0开始）
 * @return 歌词内容，如果索引无效返回NULL
 */
const char *lyrics_get_line(int index);

/**
 * @brief 获取当前播放时间对应的歌词行索引
 * @param current_time_ms 当前播放时间（毫秒）
 * @return 当前行索引
 */
int lyrics_get_current_line_index(int current_time_ms);

#ifdef __cplusplus
}
#endif

#endif /* LYRICS_PARSER_H */