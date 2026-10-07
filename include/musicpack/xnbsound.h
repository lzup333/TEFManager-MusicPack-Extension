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
#ifndef XNBSOUND_H
#define XNBSOUND_H
#include <stdint.h>
#include <stddef.h>

typedef struct {
    unsigned char* format;
    int format_len;
    unsigned char* waveform;
    int waveform_len;
    uint16_t format_tag;
    uint16_t channel_count;
    uint32_t sample_rate;
    uint32_t avg_bytes_per_sec;
    uint16_t block_align;
    uint16_t bits_per_sample;
    int32_t loop_start;
    int32_t loop_length;
    int32_t duration_ms;
} xnb_sound_t;

int xnb_parse_sound(const char* path, xnb_sound_t* out, char* err, int errcap);
void xnb_sound_free(xnb_sound_t* s);
int xnb_normalize_pcm16(const xnb_sound_t* s, unsigned char** out, int* out_bytes, int* out_rate, int* out_channels, char* err, int errcap);
#endif
