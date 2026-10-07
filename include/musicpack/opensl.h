/*
 * MusicPack Extension（优化版）
 * 基于 qingqing114514/qingshemg 的 MusicPack Extension（MIT）二次开发。
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Copyright (c) 2026 qingqing114514（原项目，MIT，全文见 LICENSE-MIT）
 * Copyright (c) 2026 lzup（本二开版本，AGPL-3.0）
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
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
