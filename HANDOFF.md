# 接手说明（MusicPack Extension 优化版）

## 这是什么

以 [qingqing114514/qingshemg](https://github.com/qingqing114514/qingshemg) 的
`MusicPack Extension` **1.2.6** 为基线的**二开版本**：

- **功能/配置/目录格式与原版完全一致**（`config.json`、`music_packs/`、`sfx_packs/`）；
- `core.c` 基本未动（只改了版本号与描述）；
- **只重写了音效后端** `opensl.c`，并**重建了音效映射表** `sxfnames.h`。

当前版本 `Info.json` = **1.3.0**。

---

## 已完成

### 1. 音效后端重写（`src/opensl.c`）

原版音效延迟高的根因（读代码定位）：

| # | 位置 | 问题 |
|:--|:--|:--|
| ① | `sl_preload_all()` | `if (g_snd_n > 128) return 0;` —— 注册数超 128 **整体跳过预载**，于是每次首播都在游戏主线程同步解码（读盘 + `xnb_parse_sound` + `xnb_normalize_pcm16` + `malloc`） |
| ② | `play_vol_locked()` | 每次播放 `memcpy` 整段 PCM |
| ③ | `pool_acquire()` | 新 `(rate,ch)` 组合**现场建 OpenSL 播放器**（pitch 靠改采样率 → 组合不断新增） |
| ④ | 同上 | 每次 `Clear()`+`Enqueue`+`PLAYING`，播放器冷启动 |
| ⑤ | `SL_DEBOUNCE_MS 40` | 40ms 内同名音效被丢弃 |
| ⑥ | 同上 | 每次 `GetInterface(SL_IID_VOLUME)` |
| ⑦ | `find_snd()/sc_fetch()` | 线性扫描 + 大锁里夹着解码和 memcpy |

本版改法（API 全兼容，`core.c` 不改）：

- **注册即预载**：`sl_register()` 在导入线程就把 PCM 解码进常驻缓存；播放路径**永不**做 IO/解析/分配
- **零拷贝**：直接 `Enqueue` 常驻缓存指针；缓存项带 `inuse/end_ms`，播放期间不被 LRU 淘汰
- **播放器池预热**：按实际用到的 `(rate,ch)` 一次建好并预热（静音 buffer 打通音频通路），热路径**不建播放器**
- 队列已排空时不再 `Clear`；去抖 40ms → **8ms**（`sl_set_debounce_ms` 可调）
- 建播放器时缓存 `SLVolumeItf`
- 注册表/PCM 缓存改 **FNV-1a 哈希**，临界区只剩哈希查找 + 一次 `Enqueue`

新增可选 API：`sl_set_memory_budget()`（默认 192MB）、`sl_set_debounce_ms()`、`sl_cache_stats()`。

### 2. 音效映射表重建（`include/musicpack/sxfnames.h`）

原表 579 条里**只有前 70 条是对的**：其余是把 `SoundID` 的 506 个字段按**声明顺序**编成稠密 id
当 `type` 用，而 `PlaySound` 的 `type` 只会是 70 个 `const int`（0..69）之一
→ **按变体名命名的文件（`x_npchit26.xnb` 等）永远匹配不上**；家族前缀还缺 7 个。

现由 `tools/gen_sxfnames.py` 从 PC 源码重建：

```
fixed    = 70    （const：Dig/PlayerHit/Item/…）
named    = 508   （LegacySoundStyle 字段 → 真正 (SoundId, Style)，variations 已展开）
families = 17    （dig_ / player_hit_ / item_ / npc_hit_ / npc_killed_ / female_hit_ /
                   zombie_ / splash_ / drip_ / thunder_ / roar_ / tink_ / mech_ /
                   coin_ / coins / liquid_ / custom/）
```

查找做**双向归一化**（小写 + 去下划线），三种写法都能命中：

| 输入 | 结果 |
|:--|:--|
| `NPC_Hit_26` / `npc_hit_26` / `npchit26` | `(SoundId=3, Style=26)` |
| `Item_30` / `item30` | `(2, 30)` |
| `dd2_ballista_tower_shot` | `(42, 12)` |
| `BombFuse` / `CrowHurt` | `(42, 336)` / `(42, 328)` |

### 3. 目录整理

- 头文件与源文件分离：项目自有头 → `include/musicpack/`；`src/` 只剩 `.c`
- `Info.json` / `Manifest.json` / `BUILD.md` 提到根目录
- 新增 `.gitignore`

---

## 待办（本次刻意没做）

1. **`config.json` 加 `style` 字段**（SoundPool 路径）
   现在 config 只能给到 `type`（0..69），ogg/mp3 音效只能**整组替换**：
   即使文件名叫 `sfx_npc_hit_17.ogg`，`core.c` 里 `sfx_map_add(t,…)` 也会把 style 丢掉。
   → 需要「只换某一个变体的 ogg」时才做（约 20 行改动）。

2. **运行时 dump 校验表**
   表是从 PC 源码生成的（与移动端 `dump.cs` 的 70 const + 506 字段**完全同构**，可信），
   但有 3 处是推断：`liquid_ → 46`、`coin_ → 38`、`research_ → 63/64`。
   若要 100% 对齐，可加一段调试代码读 `Terraria.ID.SoundID.SoundByName / SoundByIndex` 打日志。

3. **源文件拆分**（暂不做）
   `core.c` 仍 2302 行，内部有注释块分隔：
   `日志 | config | 映射表 | 包导入/缓存 | 音乐(MediaPlayer) | Hook 层 | 模块入口`。
   要拆的话注意 `g_map / g_sfxmap / g_dir / g_playing_* / g_cur_music` 等 `static` 是跨块共享的，
   需要先抽内部头 + 访问器。

---

## 关键信息

| 项 | 值 |
|:--|:--|
| pkgId | `eternal.future.audiopackextension`（与官方同 id，TEFManager 的「音频包」界面直接可用） |
| 版本 | 1.3.0（> 官方 1.2.6，不会被 TEFManager 顶掉） |
| 签名指纹 | `0x8D5A0F2C4E6B1A93`（该 pkgId 的模块哈希，与官方一致） |
| 产物 | `dist/MusicPackExtension.zip`（`Info.json` + `Manifest.json` + `musicpack_extension.tefpkg`） |
| tefpkg 布局 | 13 条：`[0]`文件列表 `[1]`arm64 so `[2..11]`平台占位 `[12]`LZ4 副本 |
| 模块日志 tag | `MusicPackZ`（内核日志 tag 是 `TEFKernel`） |
| 安装位置 | `<data>/module/pkg/<pkgId>.tefpkg`，且 `<data>/module/enables.txt` 里要有该 pkgId |

## 构建

```bash
export ANDROID_NDK_HOME=/path/to/android-ndk
./scripts/build_release.sh          # 若解压后没有执行位：bash scripts/build_release.sh
```

> NDK 自带的是 **x86_64 host** clang；aarch64 主机上跑不了，所以脚本用「宿主 clang + NDK sysroot」。
> 在 x86_64 机器上可以直接把脚本里的 `clang --target=...` 换成 NDK 的
> `aarch64-linux-android21-clang`（其余参数不变）。

## 验证优化是否生效

```bash
adb logcat | grep MusicPackZ | grep -E "preload|pool warm|LAZY-DECODE|cache="
```

- `preload: N ok, 0 miss, cache=… bytes` ← 预载成功（原版这里是 `preload skip: N sounds > cache 128`）
- `pool warm rate=44100 ch=2 slots=8` ← 播放器池已预热
- 不该出现 `LAZY-DECODE`（出现即仍有热路径解码）
