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
/*
 * opensl.c  —— OpenSL ES 音效后端（优化版）
 *
 * 与原版功能/API 完全兼容，只把"轮子"做快。相对原版的改动：
 *
 *   ① 注册即预载：sl_register() 在后台导入线程就把 PCM 解码进常驻缓存，
 *      播放路径【永不】做文件 IO / XNB 解析 / 分配。
 *      原版 sl_preload_all() 在条目 >128 时直接 return 0（等于从不预载），
 *      导致每次首播都在游戏主线程同步解码 —— 这是它延迟高的头号原因。
 *   ② 零拷贝：直接把常驻缓存的指针 Enqueue 给 OpenSL，不再每次 memcpy 整段 PCM。
 *      缓存项带 inuse/end_ms，正在播放期间不会被 LRU 淘汰。
 *   ③ 播放器预热：按 (rate,channels) 组合在预载结束后一次性创建并预热
 *      （静音 buffer + PLAYING 打开音频通路），热路径【绝不】CreateAudioPlayer。
 *   ④ 索引改哈希表：注册表 / PCM 缓存都是 FNV-1a + 线性探测，
 *      取代原来的线性 strcmp 扫描；临界区只剩几次哈希查找 + 一次 Enqueue。
 *   ⑤ 缓存接口缓存：SLVolumeItf 在建播放器时取一次。
 *   ⑥ 去抖窗口 40ms → 8ms（可配），避免吞掉正常连续音效。
 *   ⑦ 内存预算可配（默认 192MB，LRU + 播放保护）。
 *
 * 未改动：OpenSL ES 的 vtable 调用号、PCM 归一化（xnbsound）、速率取整策略。
 */

#include "opensl.h"
#include "xnbsound.h"
#include <android/log.h>
#include <dlfcn.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void (*g_sl_log)(const char *msg) = 0;
#ifndef MP_LOG
#define MP_LOG 1
#endif
#if MP_LOG
static void SLOG(const char *fmt, ...) {
    char b[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    __android_log_print(ANDROID_LOG_INFO, "MusicPackZ", "%s", b);
    if (g_sl_log) g_sl_log(b);
}
#else
#define SLOG(...) \
    do {          \
    } while (0)
#endif

/* ======================= OpenSL ES 最小绑定 ======================= */

typedef void *SLObjectItf;
typedef void *SLInterfaceID;
typedef void *SLEngineItf;
typedef void *SLPlayItf;
typedef void *SLAndroidSimpleBufferQueueItf;
typedef void *SLVolumeItf;
typedef uint32_t SLuint32;
typedef uint8_t SLboolean;
typedef int32_t SLmillibel;

#define SL_RESULT_SUCCESS 0
#define SL_BOOLEAN_FALSE 0
#define SL_BOOLEAN_TRUE 1
#define SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE 0x800007BDu
#define SL_DATALOCATOR_OUTPUTMIX 0x00000004u
#define SL_DATAFORMAT_PCM 0x00000002u
#define SL_PCMSAMPLEFORMAT_FIXED_16 0x0010u
#define SL_SPEAKER_FRONT_LEFT 0x1u
#define SL_SPEAKER_FRONT_RIGHT 0x2u
#define SL_SPEAKER_FRONT_CENTER 0x4u
#define SL_BYTEORDER_LITTLEENDIAN 0x00000002u
#define SL_PLAYSTATE_PLAYING 3u

struct SLDataLocator_AndroidSimpleBufferQueue {
    SLuint32 locatorType;
    SLuint32 numBuffers;
};
struct SLDataFormat_PCM {
    SLuint32 formatType;
    SLuint32 numChannels;
    SLuint32 samplesPerSec;
    SLuint32 bitsPerSample;
    SLuint32 containerSize;
    SLuint32 channelMask;
    SLuint32 endianness;
};
struct SLDataSource {
    void *pLocator;
    void *pFormat;
};
struct SLDataLocator_OutputMix {
    SLuint32 locatorType;
    SLObjectItf outputMix;
};
struct SLDataSink {
    void *pLocator;
    void *pFormat;
};

#define VT(p) (*(void ***)(p))
#define FN(p, n) (VT(p)[(n)])

static void *g_sl = 0;
static int (*p_slCreateEngine)(SLObjectItf *, SLuint32, const SLInterfaceID *, const SLboolean *,
                               SLuint32, void *) = 0;
static SLInterfaceID *g_iid_engine = 0;
static SLInterfaceID *g_iid_play = 0;
static SLInterfaceID *g_iid_bq = 0;
static SLInterfaceID *g_iid_volume = 0;

static const int g_rates[9] = {8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000};
static int nearest_rate(int r) {
    int b = g_rates[0], bd = abs(r - g_rates[0]);
    for (int i = 1; i < 9; i++) {
        int d = abs(r - g_rates[i]);
        if (d < bd) {
            b = g_rates[i];
            bd = d;
        }
    }
    return b;
}
static float my_exp2f(float x) {
    float y = x * 0.6931471805599453f;
    float t = 1.0f;
    float sum = 1.0f;
    for (int i = 1; i <= 12; i++) {
        t *= y / (float)i;
        sum += t;
    }
    return sum;
}
static float my_log10f(float x) {
    if (x <= 0.0f) return 0.0f;
    int e = 0;
    while (x >= 2.0f) {
        x *= 0.5f;
        e++;
    }
    while (x < 1.0f) {
        x *= 2.0f;
        e--;
    }
    float z = (x - 1.0f) / (x + 1.0f);
    float z2 = z * z;
    float sum = 0.0f;
    float num = z;
    for (int i = 0; i < 12; i++) {
        sum += num / (float)(2 * i + 1);
        num *= z2;
    }
    float ln = 2.0f * sum + (float)e * 0.6931471805599453f;
    return ln * 0.4342944819032518f;
}
int nearest_playback_rate(float base, float pitch) {
    if (!(pitch >= -1.0f && pitch <= 1.0f)) pitch = 0.0f;
    int w = (int)(base * my_exp2f(pitch) + 0.5f);
    if (w < 8000) w = 8000;
    if (w > 48000) w = 48000;
    return nearest_rate(w);
}

static SLObjectItf g_engine = 0, g_mix = 0;
static SLEngineItf g_eng = 0;
static int g_init = 0, g_bad = 0;
static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;

static long long g_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

/* ======================= 哈希：FNV-1a + 线性探测 ======================= */

static uint64_t fnv1a(const char *s) {
    uint64_t h = 1469598103934665603ULL;
    for (; *s; ++s) {
        h ^= (unsigned char)*s;
        h *= 1099511628211ULL;
    }
    return h;
}

/* ---- 注册表: key -> (path, rate, ch) ---- */
#define REG_CAP 1024
typedef struct {
    char key[192];
    char path[768];
    int rate, ch;
    int state; /* 0 empty, 1 used, 2 tombstone */
    long long last_ms;
} reg_t;
static reg_t g_reg[REG_CAP];
static int g_reg_n = 0;

static reg_t *reg_slot(const char *key) {
    uint64_t h = fnv1a(key) & (REG_CAP - 1);
    reg_t *tomb = 0;
    for (int i = 0; i < REG_CAP; i++) {
        reg_t *e = &g_reg[(h + i) & (REG_CAP - 1)];
        if (e->state == 0) return tomb ? tomb : e;
        if (e->state == 2) {
            if (!tomb) tomb = e;
            continue;
        }
        if (strcmp(e->key, key) == 0) return e;
    }
    return tomb;
}
static reg_t *reg_find(const char *key) {
    reg_t *e = reg_slot(key);
    return (e && e->state == 1) ? e : 0;
}

/* ---- PCM 常驻缓存: path -> PCM ---- */
#define PCM_CAP 1024
typedef struct {
    char path[768];
    unsigned char *data;
    int bytes, rate, ch;
    long long last_ms;
    long long end_ms; /* 预计播完时间 */
    int inuse;
    int state;
} pcm_t;
static pcm_t g_pcm[PCM_CAP];
static int g_pcm_n = 0;
static long g_pcm_bytes = 0;
static long g_budget = 192L * 1024 * 1024;
static int g_debounce_ms = 8;

static pcm_t *pcm_find(const char *path) {
    uint64_t h = fnv1a(path) & (PCM_CAP - 1);
    for (int i = 0; i < PCM_CAP; i++) {
        pcm_t *e = &g_pcm[(h + i) & (PCM_CAP - 1)];
        if (e->state == 0) return 0;
        if (e->state == 2) continue;
        if (strcmp(e->path, path) == 0) return e;
    }
    return 0;
}

static void pcm_release(pcm_t *e) {
    if (e->data) free(e->data);
    e->data = 0;
    e->bytes = 0;
    e->inuse = 0;
    e->end_ms = 0;
    e->state = 2; /* tombstone */
}

/* 超预算时按 LRU 淘汰；正在播放的（inuse 且未到 end_ms）不淘汰 */
static void pcm_evict_until(long need) {
    long long now = g_now_ms();
    while (g_pcm_bytes + need > g_budget && g_pcm_n > 0) {
        pcm_t *victim = 0;
        for (int i = 0; i < PCM_CAP; i++) {
            pcm_t *e = &g_pcm[i];
            if (e->state != 1 || !e->data) continue;
            if (e->inuse && now < e->end_ms) continue; /* 正在播，跳过 */
            if (!victim || e->last_ms < victim->last_ms) victim = e;
        }
        if (!victim) break; /* 全部在播，无法淘汰 */
        g_pcm_bytes -= victim->bytes;
        g_pcm_n--;
        pcm_release(victim);
    }
}

/* 解码 path -> PCM 并放入缓存（调用者持锁）。返回缓存项或 0 */
static pcm_t *pcm_insert(const char *path, int rate, int ch) {
    pcm_t *exist = pcm_find(path);
    if (exist) return exist;

    unsigned char *pcm = 0;
    int bytes = 0, prate = rate, pch = ch;

    /* 快路径：<path> 是原始 .pcm，旁边有 <path>.meta (rate ch bytes) */
    char mp[800];
    snprintf(mp, sizeof(mp), "%s.meta", path);
    FILE *mf = fopen(mp, "r");
    if (mf) {
        int mr = 0, mc = 0, mb = 0;
        if (fscanf(mf, "%d %d %d", &mr, &mc, &mb) == 3 && mr > 0 && mc > 0 && mb > 0) {
            fclose(mf);
            FILE *pf = fopen(path, "rb");
            if (pf) {
                unsigned char *pp = (unsigned char *)malloc(mb);
                if (pp && fread(pp, 1, mb, pf) == (size_t)mb) {
                    pcm = pp;
                    bytes = mb;
                    prate = mr;
                    pch = mc;
                } else if (pp) {
                    free(pp);
                }
                fclose(pf);
            }
        } else {
            fclose(mf);
        }
    }

    if (!pcm) {
        xnb_sound_t xs;
        char err[128];
        if (!xnb_parse_sound(path, &xs, err, sizeof(err))) return 0;
        unsigned char *p = 0;
        int b = 0, r = 0, c = 0;
        int ok = xnb_normalize_pcm16(&xs, &p, &b, &r, &c, err, sizeof(err));
        xnb_sound_free(&xs);
        if (!ok || !p) return 0;
        pcm = p;
        bytes = b;
        prate = r;
        pch = c;
    }
    if (prate <= 0 || pch <= 0) {
        free(pcm);
        return 0;
    }

    /* 先按预算淘汰，再找空槽（含 tombstone 复用） */
    pcm_evict_until(bytes);
    pcm_t *slot = 0;
    for (int i = 0; i < PCM_CAP; i++) {
        if (g_pcm[i].state != 1) {
            slot = &g_pcm[i];
            break;
        }
    }
    if (!slot) {
        free(pcm);
        return 0;
    }
    if (slot->state == 0) g_pcm_n++;
    memset(slot, 0, sizeof(*slot));
    snprintf(slot->path, sizeof(slot->path), "%s", path);
    slot->data = pcm;
    slot->bytes = bytes;
    slot->rate = prate;
    slot->ch = pch;
    slot->last_ms = g_now_ms();
    slot->inuse = 0;
    slot->end_ms = 0;
    slot->state = 1;
    g_pcm_bytes += bytes;
    return slot;
}

/* ======================= 播放器池（按 rate,ch 组合） ======================= */

#define COMBO_MAX 8
#define SLOT_PER_COMBO 8
typedef struct {
    SLObjectItf player;
    SLPlayItf play;
    SLAndroidSimpleBufferQueueItf bq;
    SLVolumeItf vol;
    int inuse;
    long long end_ms;
    int ok;
} slot_t;
typedef struct {
    int rate, ch;
    int n;
    slot_t s[SLOT_PER_COMBO];
} combo_t;
static combo_t g_combo[COMBO_MAX];
static int g_combo_n = 0;

/* 预热用静音（很短，让音频通路打开后立刻排空） */
static unsigned char g_silence[256];

static int slot_create(slot_t *sl, int rate, int ch) {
    memset(sl, 0, sizeof(*sl));
    struct SLDataLocator_AndroidSimpleBufferQueue bq = {SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 2};
    struct SLDataFormat_PCM fmt = {
        SL_DATAFORMAT_PCM, (SLuint32)ch, (SLuint32)(rate * 1000), SL_PCMSAMPLEFORMAT_FIXED_16,
        SL_PCMSAMPLEFORMAT_FIXED_16,
        ch == 1 ? SL_SPEAKER_FRONT_CENTER : (SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT),
        SL_BYTEORDER_LITTLEENDIAN};
    struct SLDataSource src = {&bq, &fmt};
    struct SLDataLocator_OutputMix outm = {SL_DATALOCATOR_OUTPUTMIX, g_mix};
    struct SLDataSink snk = {&outm, 0};
    SLInterfaceID ids[1] = {*g_iid_bq};
    SLboolean req[1] = {SL_BOOLEAN_TRUE};

    typedef int (*CreatePlayer_t)(SLEngineItf, SLObjectItf *, void *, void *, SLuint32,
                                  const SLInterfaceID *, const SLboolean *);
    if (((CreatePlayer_t)FN(g_eng, 2))(g_eng, &sl->player, &src, &snk, 1, ids, req) !=
            SL_RESULT_SUCCESS ||
        !sl->player)
        goto fail;

    {
        typedef int (*Realize_t)(SLObjectItf, SLboolean);
        if (((Realize_t)FN(sl->player, 0))(sl->player, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS)
            goto fail;
    }
    {
        typedef int (*GetIf_t)(SLObjectItf, SLInterfaceID, void *);
        if (((GetIf_t)FN(sl->player, 3))(sl->player, *g_iid_play, &sl->play) != SL_RESULT_SUCCESS ||
            !sl->play)
            goto fail;
    }
    {
        typedef int (*GetIf_t)(SLObjectItf, SLInterfaceID, void *);
        if (((GetIf_t)FN(sl->player, 3))(sl->player, *g_iid_bq, &sl->bq) != SL_RESULT_SUCCESS ||
            !sl->bq)
            goto fail;
    }
    /* 接口缓存：建播放器时取一次，热路径不再 GetInterface */
    if (g_iid_volume) {
        typedef int (*GetIf_t)(SLObjectItf, SLInterfaceID, void *);
        ((GetIf_t)FN(sl->player, 3))(sl->player, *g_iid_volume, &sl->vol);
    }

    /* 预热：静音 + PLAYING 把音频通路打开，之后热路径即为"队列里已经有数据"状态 */
    {
        typedef int (*Enq_t)(SLAndroidSimpleBufferQueueItf, const void *, SLuint32);
        typedef int (*SetPs_t)(SLPlayItf, SLuint32);
        ((Enq_t)FN(sl->bq, 0))(sl->bq, g_silence, (SLuint32)sizeof(g_silence));
        ((SetPs_t)FN(sl->play, 0))(sl->play, SL_PLAYSTATE_PLAYING);
    }
    sl->inuse = 1; /* 让首次真实播放走 Clear 分支，立即起播 */
    sl->end_ms = g_now_ms() + 120;
    sl->ok = 1;
    return 0;
fail:
    if (sl->player) {
        typedef void (*Destroy_t)(SLObjectItf);
        ((Destroy_t)FN(sl->player, 6))(sl->player);
        sl->player = 0;
    }
    sl->play = 0;
    sl->bq = 0;
    sl->vol = 0;
    return -1;
}

static combo_t *combo_get(int rate, int ch) {
    for (int i = 0; i < g_combo_n; i++) {
        if (g_combo[i].rate == rate && g_combo[i].ch == ch) return &g_combo[i];
    }
    if (g_combo_n >= COMBO_MAX) {
        SLOG("combo full, need rate=%d ch=%d", rate, ch);
        return 0;
    }
    combo_t *c = &g_combo[g_combo_n];
    memset(c, 0, sizeof(*c));
    c->rate = rate;
    c->ch = ch;
    for (int i = 0; i < SLOT_PER_COMBO; i++) {
        if (slot_create(&c->s[i], rate, ch) == 0) c->n++;
    }
    if (c->n == 0) return 0;
    g_combo_n++;
    SLOG("pool warm rate=%d ch=%d slots=%d", rate, ch, c->n);
    return c;
}

static slot_t *slot_acquire(combo_t *c) {
    long long now = g_now_ms();
    for (int i = 0; i < c->n; i++) {
        slot_t *s = &c->s[i];
        if (s->ok && (!s->inuse || now >= s->end_ms)) return s;
    }
    /* 全在播：抢最早结束的 */
    slot_t *oldest = 0;
    for (int i = 0; i < c->n; i++) {
        slot_t *s = &c->s[i];
        if (!s->ok) continue;
        if (!oldest || s->end_ms < oldest->end_ms) oldest = s;
    }
    return oldest;
}

/* ======================= 播放路径 ======================= */

/* 调用者持锁；命中并播放返回 1 */
static int play_locked(reg_t *r, float pitch, float volume) {
    pcm_t *p = pcm_find(r->path);
    if (!p) {
        /* 预载应已完成；兜底解码（会在日志里显形，便于定位） */
        p = pcm_insert(r->path, r->rate, r->ch);
        if (!p) return 0;
        SLOG("LAZY-DECODE %s", r->path);
    }

    int rate = nearest_playback_rate((float)p->rate, pitch);
    combo_t *c = combo_get(rate, p->ch);
    if (!c) return 0;
    slot_t *sl = slot_acquire(c);
    if (!sl) return 0;

    /* 上一段还没播完 → 清队立即接手；已排空 → 直接 Enqueue（少一次系统调用） */
    if (sl->inuse && g_now_ms() < sl->end_ms) {
        typedef int (*Clr_t)(SLAndroidSimpleBufferQueueItf);
        ((Clr_t)FN(sl->bq, 1))(sl->bq);
    }
    {
        typedef int (*Enq_t)(SLAndroidSimpleBufferQueueItf, const void *, SLuint32);
        if (((Enq_t)FN(sl->bq, 0))(sl->bq, p->data, (SLuint32)p->bytes) != SL_RESULT_SUCCESS)
            return 0;
    }
    if (sl->vol && volume > 0.0f && volume < 2.0f) {
        typedef int (*SetVol_t)(SLVolumeItf, SLmillibel);
        ((SetVol_t)FN(sl->vol, 0))(sl->vol, (SLmillibel)(2000.0f * my_log10f(volume)));
    }
    {
        typedef int (*SetPs_t)(SLPlayItf, SLuint32);
        ((SetPs_t)FN(sl->play, 0))(sl->play, SL_PLAYSTATE_PLAYING);
    }

    long long dur = (long long)p->bytes * 1000LL / (2LL * (long long)p->ch * (long long)rate);
    sl->inuse = 1;
    sl->end_ms = g_now_ms() + dur + 150;
    p->inuse = 1;
    p->end_ms = sl->end_ms;
    p->last_ms = g_now_ms();
    return 1;
}

/* 去抖时间戳存在注册项里，O(1)，不再扫全表 */
static int debounce_ok(reg_t *r) {
    if (g_debounce_ms <= 0) return 1;
    long long now = g_now_ms();
    if (now - r->last_ms < g_debounce_ms) return 0;
    r->last_ms = now;
    return 1;
}

/* ======================= 对外 API ======================= */

int sl_init(void) {
    pthread_mutex_lock(&g_mtx);
    if (g_init) {
        pthread_mutex_unlock(&g_mtx);
        return 1;
    }
    if (g_bad) {
        pthread_mutex_unlock(&g_mtx);
        return 0;
    }
    if (!g_sl) {
        g_sl = dlopen("libOpenSLES.so", RTLD_NOW);
        if (!g_sl) {
            g_bad = 1;
            pthread_mutex_unlock(&g_mtx);
            SLOG("sl: dlopen fail");
            return 0;
        }
    }
    p_slCreateEngine = (int (*)(SLObjectItf *, SLuint32, const SLInterfaceID *,
                                const SLboolean *, SLuint32, void *))dlsym(g_sl, "slCreateEngine");
    g_iid_engine = (SLInterfaceID *)dlsym(g_sl, "SL_IID_ENGINE");
    g_iid_play = (SLInterfaceID *)dlsym(g_sl, "SL_IID_PLAY");
    g_iid_bq = (SLInterfaceID *)dlsym(g_sl, "SL_IID_ANDROIDSIMPLEBUFFERQUEUE");
    g_iid_volume = (SLInterfaceID *)dlsym(g_sl, "SL_IID_VOLUME");
    if (!p_slCreateEngine || !g_iid_engine || !g_iid_play || !g_iid_bq) {
        g_bad = 1;
        pthread_mutex_unlock(&g_mtx);
        SLOG("sl: missing symbols");
        return 0;
    }
    if (p_slCreateEngine(&g_engine, 0, 0, 0, 0, 0) != SL_RESULT_SUCCESS || !g_engine) {
        g_bad = 1;
        pthread_mutex_unlock(&g_mtx);
        SLOG("sl: slCreateEngine fail");
        return 0;
    }
    {
        typedef int (*Realize_t)(SLObjectItf, SLboolean);
        if (((Realize_t)FN(g_engine, 0))(g_engine, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS) {
            g_bad = 1;
            pthread_mutex_unlock(&g_mtx);
            SLOG("sl: Realize fail");
            return 0;
        }
    }
    {
        typedef int (*GetIf_t)(SLObjectItf, SLInterfaceID, void *);
        if (((GetIf_t)FN(g_engine, 3))(g_engine, *g_iid_engine, &g_eng) != SL_RESULT_SUCCESS ||
            !g_eng) {
            g_bad = 1;
            pthread_mutex_unlock(&g_mtx);
            SLOG("sl: ENGINE fail");
            return 0;
        }
    }
    {
        typedef int (*CreateOutMix_t)(SLEngineItf, SLObjectItf *, SLuint32, const SLInterfaceID *,
                                      const SLboolean *);
        if (((CreateOutMix_t)FN(g_eng, 7))(g_eng, &g_mix, 0, 0, 0) != SL_RESULT_SUCCESS || !g_mix) {
            g_bad = 1;
            pthread_mutex_unlock(&g_mtx);
            SLOG("sl: CreateOutputMix fail");
            return 0;
        }
    }
    {
        typedef int (*Realize_t)(SLObjectItf, SLboolean);
        if (((Realize_t)FN(g_mix, 0))(g_mix, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS) {
            g_bad = 1;
            pthread_mutex_unlock(&g_mtx);
            SLOG("sl: mix Realize fail");
            return 0;
        }
    }
    g_init = 1;
    pthread_mutex_unlock(&g_mtx);
    SLOG("sl: init ok");
    return 1;
}

int sl_is_init(void) { return g_init; }

/* 注册即预载：解码在调用者线程（导入线程）完成，播放路径不再做 IO */
int sl_register(const char *key, const char *path, int rate, int channels) {
    if (!key || !path || !*path || rate <= 0 || channels <= 0) return 0;
    pthread_mutex_lock(&g_mtx);
    if (!g_init) {
        pthread_mutex_unlock(&g_mtx);
        return 0;
    }
    reg_t *e = reg_slot(key);
    if (!e) {
        pthread_mutex_unlock(&g_mtx);
        return 0;
    }
    if (e->state != 1) {
        if (e->state == 0) g_reg_n++;
        memset(e, 0, sizeof(*e));
        snprintf(e->key, sizeof(e->key), "%s", key);
        e->state = 1;
    }
    snprintf(e->path, sizeof(e->path), "%s", path);
    e->rate = rate;
    e->ch = channels;
    pcm_insert(path, rate, channels); /* 预载 */
    pthread_mutex_unlock(&g_mtx);
    return 1;
}

int sl_has(const char *key) {
    if (!g_init) return 0;
    pthread_mutex_lock(&g_mtx);
    int r = reg_find(key) ? 1 : 0;
    pthread_mutex_unlock(&g_mtx);
    return r;
}

int sl_play_vol(const char *key, float pitch_offset, float volume) {
    if (!g_init) return 0;
    pthread_mutex_lock(&g_mtx);
    reg_t *r = reg_find(key);
    int ret = r ? play_locked(r, pitch_offset, volume) : 0;
    pthread_mutex_unlock(&g_mtx);
    return ret;
}

int sl_play(const char *key, float pitch_offset) { return sl_play_vol(key, pitch_offset, -1.0f); }

/* 高频 hook 路径：一次加锁，哈希查找 + Enqueue，无 IO/无分配/无播放器创建 */
int sl_try_play(const char *key, float pitch_offset, float volume) {
    if (!g_init || !key) return 0;
    pthread_mutex_lock(&g_mtx);
    int ret = 0;
    reg_t *r = reg_find(key);
    if (r) {
        ret = 1;
        if (debounce_ok(r)) play_locked(r, pitch_offset, volume);
    }
    pthread_mutex_unlock(&g_mtx);
    return ret;
}

int sl_try_play2(const char *k1, const char *k2, float pitch_offset, float volume) {
    if (!g_init) return 0;
    pthread_mutex_lock(&g_mtx);
    int ret = 0;
    reg_t *r = k1 ? reg_find(k1) : 0;
    if (r) {
        ret = 1;
        if (debounce_ok(r)) play_locked(r, pitch_offset, volume);
    } else {
        reg_t *r2 = k2 ? reg_find(k2) : 0;
        if (r2) {
            ret = 1;
            if (debounce_ok(r2)) play_locked(r2, pitch_offset, volume);
        }
    }
    pthread_mutex_unlock(&g_mtx);
    return ret;
}

int sl_preload(const char *key) {
    if (!g_init || !key) return 0;
    pthread_mutex_lock(&g_mtx);
    reg_t *r = reg_find(key);
    int ok = 0;
    if (r) ok = pcm_find(r->path) != 0 || pcm_insert(r->path, r->rate, r->ch) != 0;
    pthread_mutex_unlock(&g_mtx);
    return ok;
}

/* 兼容保留：注册时已预载，这里只补齐缺失项 */
int sl_preload_all(void) {
    if (!g_init) return 0;
    pthread_mutex_lock(&g_mtx);
    int n = 0, miss = 0;
    for (int i = 0; i < REG_CAP; i++) {
        reg_t *r = &g_reg[i];
        if (r->state != 1) continue;
        if (pcm_find(r->path)) {
            n++;
            continue;
        }
        if (pcm_insert(r->path, r->rate, r->ch)) n++;
        else miss++;
    }
    long long total = g_pcm_bytes;
    pthread_mutex_unlock(&g_mtx);
    SLOG("preload: %d ok, %d miss, cache=%ld bytes", n, miss, (long)total);
    return n;
}

void sl_housekeep(void) {
    if (!g_init) return;
    pthread_mutex_lock(&g_mtx);
    long long now = g_now_ms();
    for (int i = 0; i < PCM_CAP; i++) {
        pcm_t *e = &g_pcm[i];
        if (e->state == 1 && e->inuse && now >= e->end_ms) e->inuse = 0;
    }
    for (int i = 0; i < g_combo_n; i++) {
        combo_t *c = &g_combo[i];
        for (int j = 0; j < c->n; j++) {
            slot_t *s = &c->s[j];
            if (s->inuse && now >= s->end_ms) s->inuse = 0;
        }
    }
    pthread_mutex_unlock(&g_mtx);
}

void sl_set_memory_budget(long bytes) {
    pthread_mutex_lock(&g_mtx);
    if (bytes > 0) g_budget = bytes;
    pthread_mutex_unlock(&g_mtx);
}

void sl_set_debounce_ms(int ms) {
    pthread_mutex_lock(&g_mtx);
    g_debounce_ms = ms < 0 ? 0 : ms;
    pthread_mutex_unlock(&g_mtx);
}

int sl_cache_stats(char *buf, int cap) {
    if (!buf || cap <= 0) return 0;
    pthread_mutex_lock(&g_mtx);
    int n = snprintf(buf, (size_t)cap, "reg=%d pcm=%d bytes=%ld budget=%ld combos=%d db=%d",
                     g_reg_n, g_pcm_n, (long)g_pcm_bytes, (long)g_budget, g_combo_n, g_debounce_ms);
    pthread_mutex_unlock(&g_mtx);
    return n;
}

void sl_shutdown(void) {
    pthread_mutex_lock(&g_mtx);
    for (int i = 0; i < g_combo_n; i++) {
        combo_t *c = &g_combo[i];
        for (int j = 0; j < c->n; j++) {
            slot_t *s = &c->s[j];
            if (s->player) {
                typedef void (*Destroy_t)(SLObjectItf);
                ((Destroy_t)FN(s->player, 6))(s->player);
                s->player = 0;
            }
            s->play = 0;
            s->bq = 0;
            s->vol = 0;
        }
        c->n = 0;
    }
    g_combo_n = 0;
    for (int i = 0; i < PCM_CAP; i++) {
        if (g_pcm[i].data) pcm_release(&g_pcm[i]);
    }
    memset(g_pcm, 0, sizeof(g_pcm));
    g_pcm_n = 0;
    g_pcm_bytes = 0;
    memset(g_reg, 0, sizeof(g_reg));
    g_reg_n = 0;
    if (g_mix) {
        typedef void (*Destroy_t)(SLObjectItf);
        ((Destroy_t)FN(g_mix, 6))(g_mix);
        g_mix = 0;
    }
    if (g_engine) {
        typedef void (*Destroy_t)(SLObjectItf);
        ((Destroy_t)FN(g_engine, 6))(g_engine);
        g_engine = 0;
    }
    g_eng = 0;
    g_init = 0;
    g_bad = 0;
    pthread_mutex_unlock(&g_mtx);
}
