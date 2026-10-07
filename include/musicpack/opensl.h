#ifndef OPENSL_H
#define OPENSL_H

/* registered key -> normalized 16bit PCM, played via OpenSL ES */
int  sl_init(void);
int  sl_register(const char* key, const char* path, int rate, int channels);
int  sl_has(const char* key);
int  sl_play(const char* key, float pitch_offset);
int  sl_play_vol(const char* key, float pitch_offset, float volume);
/* 一次加锁完成: 判断存在+取PCM+播放; 命中返回1, 未注册/失败返回0. 用于高频 hook 路径减锁 */
int  sl_try_play(const char* key, float pitch_offset, float volume);
int  sl_try_play2(const char* k1, const char* k2, float pitch_offset, float volume);
int  sl_preload(const char* key);
int  sl_preload_all(void);
void sl_shutdown(void);
void sl_housekeep(void);
int  sl_is_init(void);

/* ---- 优化新增（可选，不调用则用默认值） ---- */
/* PCM 常驻缓存预算（字节），默认 192MB；超出按 LRU 淘汰（正在播放的不淘汰） */
void sl_set_memory_budget(long bytes);
/* 同名音效去抖窗口（毫秒），默认 8；设为 0 关闭 */
void sl_set_debounce_ms(int ms);
/* 输出一行统计到 buf，返回写入长度 */
int  sl_cache_stats(char* buf, int cap);

#endif
