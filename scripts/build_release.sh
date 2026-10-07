#!/usr/bin/env bash
# =============================================================================
# MusicPack Extension（优化版）构建 + 打包
#
#   1) 宿主 clang + NDK sysroot 交叉编译 arm64
#      （NDK 只带 x86_64 clang，aarch64 主机跑不了；x86_64 主机可用官方 NDK 预设）
#   2) 校验 UND 里不残留 patchlib_/tefstd_（残留则加载时内核报 Failed to open dynamic library）
#   3) tools/pack_module 打包成与官方完全一致的 13 条 tefpkg
#      （新版 TEFPkg-Tool 会把 libmodule.* 排除，只出 12 条，故自建打包器）
#   4) 套 Info.json + Manifest.json 成 MusicPackExtension.zip
#
# 依赖：ANDROID_NDK_HOME
# =============================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

: "${ANDROID_NDK_HOME:?请导出 ANDROID_NDK_HOME}"
FINGERPRINT="${MODULE_FINGERPRINT:-0x8D5A0F2C4E6B1A93}"   # 与官方一致

TC="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64"
SYS="$TC/sysroot"
RD="$TC/lib/clang/17"
TARGET="aarch64-linux-android21"
CFLAGS="-std=c17 -DNDEBUG -DMP_LOG=${MP_LOG:-1} -Wno-incompatible-function-pointer-types -Iinclude/musicpack -Iinclude -Isrc"

BUILD=build/arm64
rm -rf "$BUILD"; mkdir -p "$BUILD" dist/pack

echo "==> 编译模块"
OBJS=""
for f in src/core.c src/miniz.c src/opensl.c src/xnbsound.c \
         include/tefkernel-cpp-wrapper/tefkernel/tef_api_imp.c; do
    o="$BUILD/$(echo "$f" | tr '/' '_').o"
    clang --target="$TARGET" --sysroot="$SYS" -resource-dir="$RD" $CFLAGS -O2 -fPIC -c "$f" -o "$o"
    OBJS="$OBJS $o"
done
SO="$BUILD/libmodule.android.arm64.so"
clang --target="$TARGET" --sysroot="$SYS" -resource-dir="$RD" -rtlib=compiler-rt \
      -shared -fPIC $OBJS -o "$SO" -llog -ldl

STRIP="$(command -v llvm-strip || ls /usr/lib/llvm-*/bin/llvm-strip 2>/dev/null | head -1 || true)"
[ -n "$STRIP" ] && "$STRIP" --strip-all "$SO" || true

echo "==> 检查未定义符号"
if nm -D --undefined-only "$SO" | grep -qE "patchlib_|tefstd_|terraria_|memdl_|tefpkg_"; then
    echo "错误：UND 里残留 TEF 符号（必须一起编译 tef_api_imp.c）" >&2
    exit 1
fi
readelf -d "$SO" | grep NEEDED

cp "$SO" dist/pack/

echo "==> 编译打包器"
PB="$BUILD/packer"
PB_OBJS=""
for f in tools/tefpkg/tefpkg.c tools/tefpkg/lz4.c tools/tefpkg/lz4hc.c \
         tools/tefpkg/siphash.c tools/tefpkg/compressor.c; do
    o="$BUILD/$(basename "$f").o"
    clang -std=c17 -D_GNU_SOURCE -I tools/tefpkg -O2 -c "$f" -o "$o"
    PB_OBJS="$PB_OBJS $o"
done
clang -std=c17 -D_GNU_SOURCE -I tools/tefpkg -O2 -c tools/pack_module.c -o "$BUILD/pack_module.o"
clang "$BUILD/pack_module.o" $PB_OBJS -o "$PB"

echo "==> 打包 tefpkg（13 条，与官方布局一致）"
"$PB" dist/pack/libmodule.android.arm64.so dist/musicpack_extension.tefpkg "$FINGERPRINT"

cp Info.json Manifest.json dist/
python3 - <<'PY'
import zipfile, os
os.chdir("dist")
with zipfile.ZipFile("MusicPackExtension.zip", "w", zipfile.ZIP_DEFLATED) as z:
    for n in ("Info.json", "Manifest.json", "musicpack_extension.tefpkg"):
        z.write(n, n)
PY

echo
ls -la dist/MusicPackExtension.zip dist/musicpack_extension.tefpkg
