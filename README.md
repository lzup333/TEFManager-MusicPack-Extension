# MusicPack Extension（音效后端优化版）

以 [qingqing114514/qingshemg](https://github.com/qingqing114514/qingshemg) 的
`MusicPack Extension` 1.2.6 为**基线**，保持功能与配置格式完全不变，**只重写 OpenSL 音效后端**。

> 不重复造轮子 —— 基础功能（音乐/音效替换、创意工坊包导入、MediaPlayer 音乐淡入淡出状态机）沿用原版；
> 只把「轮子」做快。

---

## 原版音效延迟高在哪（读代码定位，逐条）

| # | 位置 | 问题 |
|:--|:--|:--|
| ① | `opensl.c: sl_preload_all()` | `if (g_snd_n > SND_CACHE_MAX) return 0;` —— 注册音效超过 **128 条就整体跳过预载**。实际注册数远超 128（`sxfnames.h` 600 条 + 家族展开），**等于从不预载**，于是每次首播都在游戏主线程同步走 `sc_fetch()` → 读盘 + `xnb_parse_sound` + `xnb_normalize_pcm16` + `malloc` |
| ② | `opensl.c: play_vol_locked()` | 每次播放 `memcpy(p->bufs[bi], sptr, sb)` —— 整段 PCM 拷进播放器缓冲 |
| ③ | `opensl.c: pool_acquire()` | 命中新 `(rate, channels)` 组合时**现场** `CreateAudioPlayer` + `Realize`（ms 级）；而 pitch 是**改采样率**实现的，导致组合不断新增 |
| ④ | `opensl.c: play_vol_locked()` | 每次 `BufferQueue.Clear()` + `Enqueue` + `SetPlayState(PLAYING)`，播放器冷启动 |
| ⑤ | `opensl.c: SL_DEBOUNCE_MS 40` | 40 ms 内的同名音效被直接丢弃 |
| ⑥ | `opensl.c: play_vol_locked()` | 每次 `GetInterface(SL_IID_VOLUME)` |
| ⑦ | `opensl.c: find_snd()/sc_fetch()` | 线性 `strcmp` 扫描（最多 512/128 项）+ 全程持全局锁，且临界区里包含解码与 memcpy |
| ⑧ | `core.c: sfx_prefix()` | OpenSL 与 SoundPool 两条路都要先查表 |

## 本版改动（`src/opensl.c` 重写，API 不变）

| # | 改法 |
|:--|:--|
| ① | **注册即预载**：`sl_register()` 在导入线程就把 PCM 解码进常驻缓存；播放路径**永不**做 IO/解析/分配。保留 `sl_preload_all()` 作为补齐口 |
| ② | **零拷贝**：直接把常驻缓存指针 `Enqueue` 给 OpenSL；缓存项带 `inuse/end_ms`，**播放期间不被 LRU 淘汰** |
| ③ | **播放器池预热**：按实际用到的 `(rate,ch)` 组合一次性建好并预热（静音 buffer + PLAYING 打通音频通路），热路径**绝不创建播放器** |
| ④ | 空闲（队列已排空）时**不再 Clear**，只在抢占在播槽位时 Clear；`SetPlayState` 幂等 |
| ⑤ | 去抖 40 ms → **8 ms**（`sl_set_debounce_ms()` 可调，0 = 关闭）；时间戳存在注册项里，O(1) |
| ⑥ | 建播放器时**缓存 `SLVolumeItf`** |
| ⑦ | 注册表 / PCM 缓存改 **FNV-1a + 线性探测哈希**；临界区只剩几次哈希查找 + 一次 `Enqueue` |
| ⑧ | 保持单一 OpenSL 路径优先（SoundPool 逻辑未动，兼容原版） |

新增可选 API（不调用则用默认值）：

```c
void sl_set_memory_budget(long bytes);  /* PCM 常驻缓存预算，默认 192MB，LRU + 播放保护 */
void sl_set_debounce_ms(int ms);        /* 默认 8 */
int  sl_cache_stats(char* buf, int cap);/* 一行统计，便于打日志 */
```

**兼容性**：`opensl.h` 全部原函数签名不变，`core.c` 无需改动；`config.json` / `music_packs/` / `sfx_packs/` 目录与格式与原版完全一致。

---

## 目录结构

```
MusicPack-Extension/
├── src/                          # 只有 .c
│   ├── core.c                    # 模块入口 / config / 包导入 / 音乐 / Hook
│   ├── opensl.c                  # OpenSL ES 音效后端（本版重写）
│   ├── xnbsound.c                # XNB SoundEffectReader -> PCM
│   └── miniz.c                   # vendored
├── include/
│   ├── musicpack/                # 项目自有头文件（与 src 分离）
│   │   ├── jni_helper.h  mkdirs.h  opensl.h  xnbsound.h
│   │   └── sxfnames.h            # 由 tools/gen_sxfnames.py 生成
│   ├── lib/miniz.h               # vendored
│   └── tefkernel-cpp-wrapper/    # vendored（TEFKernel C 接口）
├── tools/                        # pack_module（打 tefpkg）/ gen_sxfnames.py / tefpkg 源码
├── docs/                         # MusicID_zh.txt / SoundID_zh.csv
├── scripts/build_release.sh
├── Info.json  Manifest.json  BUILD.md  README.md  LICENSE  LICENSE-MIT
└── dist/                         # 构建产物（MusicPackExtension.zip）
```

> `core.c` 内部用注释块按子系统分隔（日志 / config / 映射表 / 包导入 / 音乐 / Hook），
> 本次只做了目录归位；源文件拆分待定。

## 构建

```bash
export ANDROID_NDK_HOME=/path/to/android-ndk
./scripts/build_release.sh
```

脚本做的事：

1. 宿主 `clang --target=aarch64-linux-android21` + NDK sysroot 交叉编译
   （NDK 只带 x86_64 clang，aarch64 主机跑不了；x86_64 主机可直接用 NDK 自带 clang）
2. 校验 `nm -D --undefined-only`：不得残留 `patchlib_*` / `tefstd_*`
   （必须一起编译 `tef_api_imp.c`，否则加载时内核报 `Failed to open dynamic library`）
3. `tools/pack_module` 打 **13 条** tefpkg，布局与官方发布包逐条一致：
   ```
   [0]  文件列表（id=12 -> libmodule.android.arm64.so）
   [1]  arm64 so（不压缩）      <- 内核按 TEFPKG_ID_DYLIB 取这条
   [2..11] 其它平台占位
   [12] so 的 LZ4 压缩副本
   ```
   > 新版 TEFPkg-Tool 会把 `libmodule.*` 放进 exclude，只产出 12 条，故内置 `tools/pack_module`（直接调 tefpkg API）。
4. 套 `Info.json` + `Manifest.json` → `dist/MusicPackExtension.zip`

产物：

```
dist/
├── MusicPackExtension.zip      # 发布包
├── musicpack_extension.tefpkg  # 13 条，签名 0x8D5A0F2C4E6B1A93
├── Info.json                   # version 1.3.0
└── Manifest.json
```

## 上线后怎么确认优化生效

```bash
adb logcat | grep -E "MusicPackZ" | grep -E "preload|pool warm|LAZY-DECODE|cache="
```

- `preload: N ok, 0 miss, cache=... bytes` —— 预载成功（原版会打 `preload skip: N sounds > cache 128`）
- `pool warm rate=44100 ch=2 slots=8` —— 播放器池已预热
- **不应**出现 `LAZY-DECODE`（出现说明仍在热路径解码，需排查）

---

## 音效映射表（sxfnames.h）已重建

原表的问题：`g_sfx_fixed` 把 `Terraria.ID.SoundID` 的字段按**声明顺序**编成 0..578 的稠密 id 当 `type`，
但 `PlaySound` 的 `type` 只会是 SoundID 的 **70 个 `const int`（0..69）** 之一（`Item/NPCHit/...`），
变体靠 `style` 区分。于是那 509 条稠密 id Hook 永远收不到 ——
**按变体名命名的文件（如 `x_npchit26.xnb`）永远匹配不上**，只有 `x_NPC_Hit_26.xnb` 那种
带下划线的写法能靠家族前缀命中，而家族前缀本身还缺 7 个（`dig_ / player_hit_ / female_hit_ /
thunder_ / tink_ / mech_ / research_ / custom/`）。

现在由 `tools/gen_sxfnames.py` 从 PC 源码重建：

```bash
python3 tools/gen_sxfnames.py \
  <PC>/Terraria.ID/SoundID.cs \
  <PC>/Terraria.Audio/LegacySoundPlayer.cs \
  include/musicpack/sxfnames.h
```

产出三张表并**双向归一化**（小写 + 去下划线）后比较，三种写法都能命中：

| 输入 | 解析结果 |
|:--|:--|
| `NPC_Hit_26` / `npc_hit_26` / `npchit26` | `(SoundId=3, Style=26)` |
| `Item_30` / `item30` | `(2, 30)` |
| `Dig_0` | `(0, 0)` |
| `Zombie_5` | `(29, 5)` |
| `Thunder_2` | `(43, 2)` |
| `dd2_ballista_tower_shot` / `DD2_BallistaTowerShot` | `(42, 12)`（trackable） |
| `PlayerKilled` / `Moonlord` / `Bombfuse` | `(5, -1)` / `(41, -1)` / `(42, 336)` |

规模：`fixed=70`（const）、`named=508`（LegacySoundStyle 字段，variations 已展开）、
`families=17`（`dig_ / player_hit_ / item_ / npc_hit_ / npc_killed_ / female_hit_ / zombie_ /
splash_ / drip_ / thunder_ / roar_ / tink_ / mech_ / coin_ / coins / liquid_ / custom/`）。

### config.json 的 `style` 字段（ogg/mp3 音效精确到变体）

`config.json` 里的 `type` 走 **SoundPool** 路径（ogg/mp3），取值是 `PlaySound` 的 `type`，
**只有 0..69 有意义**。要精确到某个变体，加 `style`：

```json
[
  { "enable": true, "type": 3, "style": 26, "file": "hit26.ogg" },   // 只换 NPC 受击 #26
  { "enable": true, "type": 3,             "file": "hit_all.ogg" }   // 换全部 NPC 受击
]
```

| `style` | 含义 |
|:--|:--|
| 省略 / `-1` | 匹配该 `type` 的**全部**变体（旧行为，向后兼容） |
| `>= 0` | 只匹配该变体 |

匹配优先级：**精确 `(type,style)` > 通配 `(type,-1)`**，即写了 `hit_all.ogg` 又写了
`hit26.ogg` 时，#26 用后者、其余用前者。

同样的修正也作用于包内文件：`sfx_NPC_Hit_26.ogg`（或 `sfx_npchit26.ogg`）现在只会替换
**#26**，而不再像之前那样把整个 type 都顶掉（旧代码在 `sfx_map_add()` 处把 style 丢了）。

> xnb 音效走 OpenSL 路径，选择器就是文件名（`x_<名字>.xnb`），本来就支持精确到变体。

## 许可证

本 fork 采用 **GNU Affero General Public License v3.0（AGPL-3.0）**，
完整全文见 [`LICENSE`](LICENSE)。

本项目基于 [qingqing114514/qingshemg](https://github.com/qingqing114514/qingshemg) 的
`MusicPack Extension` 二次开发，原项目采用 **MIT License**（Copyright (c) 2026
qingqing114514）。根据 MIT 的要求，原项目的版权声明与许可全文已完整保留在本仓库
[`LICENSE-MIT`](LICENSE-MIT) 中。

> MIT 与 AGPL-3.0 兼容：允许将 MIT 代码并入以 AGPL-3.0 发布的衍生作品，
> 只需保留原版权声明与 MIT 许可全文（见上）。

本模块作为 [TEFManager](https://github.com/eternalfuture-e38299/TEFManager)
（本身同样以 AGPL-3.0 发布）的扩展，并链接了含 AGPL-3.0 代码的 TEFKernel，
故采用 AGPL-3.0 与所属框架的许可保持一致。

### 第三方组件

`include/tefkernel-cpp-wrapper/`（TEFKernel C 接口）大部分文件为 MIT
（Copyright (c) eternalfuture-e38299），`tefpkg` 相关文件为 AGPL-3.0；
`include/lib/miniz.h`、`src/miniz.c` 采用各自文件头部声明的许可（unlicense / MIT）。
这些文件的原始许可声明保留在原文件中。
