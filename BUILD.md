# 编译说明

## 目录布局

```
MusicPack-Extension/
├── src/                     # 只有 .c
│   ├── core.c               # 模块入口 / config / 包导入 / 音乐 / Hook
│   ├── opensl.c             # OpenSL ES 音效后端（优化版）
│   ├── xnbsound.c           # XNB SoundEffectReader -> PCM
│   └── miniz.c              # vendored（解包）
├── include/
│   ├── musicpack/           # 项目自有头文件
│   │   ├── jni_helper.h  mkdirs.h  opensl.h  xnbsound.h
│   │   └── sxfnames.h       # 由 tools/gen_sxfnames.py 生成
│   ├── lib/miniz.h          # vendored
│   └── tefkernel-cpp-wrapper/   # vendored（TEFKernel C 接口）
├── tools/                   # pack_module（打 tefpkg）+ gen_sxfnames.py
├── docs/                    # MusicID_zh.txt / SoundID_zh.csv
├── scripts/build_release.sh # 一键编译 + 打包
├── Info.json  Manifest.json BUILD.md  README.md  LICENSE  LICENSE-MIT
```

## 编译（手动）

```sh
NDK=$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64
clang --target=aarch64-linux-android21 --sysroot=$NDK/sysroot \
      -resource-dir=$NDK/lib/clang/17 -rtlib=compiler-rt \
      -std=c17 -DNDEBUG -DMP_LOG=1 -Wno-incompatible-function-pointer-types \
      -Iinclude/musicpack -Iinclude -Isrc \
      -shared -fPIC -O2 \
      src/core.c src/miniz.c src/opensl.c src/xnbsound.c \
      include/tefkernel-cpp-wrapper/tefkernel/tef_api_imp.c \
      -o libmodule.android.arm64.so -llog -ldl
```

> 官方 NDK 自带的是 **x86_64 host** clang；在 aarch64 主机上跑不了，所以用宿主 clang + NDK sysroot（`scripts/build_release.sh` 已封装）。

## 打包（13 条，与官方布局一致）

新版 TEFPkg-Tool 会把 `libmodule.*` 放进 exclude，只产出 12 条，因此内置 `tools/pack_module`：

```sh
tools/pack_module <so> musicpack_extension.tefpkg 0x8D5A0F2C4E6B1A93
# [0]文件列表 [1]arm64 so [2..11]平台占位 [12]LZ4 副本
```

## 注意

- **必须带 `-DNDEBUG`**。
- **必须把 `tef_api_imp.c` 一起编译**，否则 `patchlib_*` 会变成 UND，内核加载时报 `Failed to open dynamic library`。
  编译后自查：`nm -D --undefined-only x.so | grep -E "patchlib_|tefstd_"` 应为空。
- 重新生成音效表：
  ```sh
  python3 tools/gen_sxfnames.py <PC>/Terraria.ID/SoundID.cs \
        <PC>/Terraria.Audio/LegacySoundPlayer.cs include/musicpack/sxfnames.h
  ```
