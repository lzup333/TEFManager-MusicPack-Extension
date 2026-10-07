/*
 * pack_module.c —— 生成与官方 MusicPack_Extension.tefpkg 完全一致的 13 条布局
 *
 *   [0]  文件列表（1 条: id=12 libmodule.<platform>.<arch>.so），不压缩
 *   [1]  arm64 模块 so（不压缩）        <- 内核按 TEFPKG_ID_DYLIB 取这一条
 *   [2..11] 其它平台占位（1 字节 0xFF，不压缩）
 *   [12] 模块 so 的 LZ4 压缩副本（文件列表指向它）
 *
 * 新版 TEFPkg-Tool 会把 libmodule.* 放进 exclude，导致只产出 12 条；
 * 这里直接调 tefpkg API 复刻官方布局，保证与线上可用包一致。
 *
 * 用法: pack_module <so路径> <输出tefpkg> [fingerprint]
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "tefpkg.h"

#define PLATFORMS 11

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <so> <out.tefpkg> [fingerprint]\n", argv[0]);
        return 2;
    }
    const char *so = argv[1];
    const char *out = argv[2];
    uint64_t fingerprint = (argc > 3) ? strtoull(argv[3], NULL, 0) : 0;

    /* ---- 文件列表: count=1, "<name>", id=12 ---- */
    const char *name = strrchr(so, '/');
    name = name ? name + 1 : so;

    uint8_t list[4 + 2 + 512 + 4];
    size_t off = 0;
    int cnt = 1;
    memcpy(list + off, &cnt, 4); off += 4;
    uint16_t nlen = (uint16_t)strlen(name);
    memcpy(list + off, &nlen, 2); off += 2;
    memcpy(list + off, name, nlen); off += nlen;
    int list_id = 12;
    memcpy(list + off, &list_id, 4); off += 4;

    tefpkg_t *pkg = NULL;
    if (tefpkg_create_reserved_from_file(out, 13, &pkg) != TEF_OK || !pkg) {
        fprintf(stderr, "create reserved failed\n");
        return 1;
    }

    /* [0] 文件列表 */
    if (tefpkg_add_entry_from_memory(pkg, COMPRESS_NONE, 0, list, (uint32_t)off) != TEF_OK) {
        fprintf(stderr, "add file list failed\n");
        return 1;
    }

    /* [1] arm64 so（不压缩） */
    if (tefpkg_add_entry_from_file(pkg, so, COMPRESS_NONE, 0) != TEF_OK) {
        fprintf(stderr, "add so failed\n");
        return 1;
    }

    /* [2..11] 其余平台占位 */
    uint8_t empty[1] = {255};
    for (int i = 1; i < PLATFORMS; i++) {
        if (tefpkg_add_entry_from_memory(pkg, COMPRESS_NONE, 0, empty, 1) != TEF_OK) {
            fprintf(stderr, "add placeholder %d failed\n", i + 1);
            return 1;
        }
    }

    /* [12] LZ4 压缩副本 */
    if (tefpkg_add_entry_from_file(pkg, so, COMPRESS_LZ4, 1) != TEF_OK) {
        fprintf(stderr, "add compressed copy failed\n");
        return 1;
    }

    if (tefpkg_sign_package(pkg, fingerprint) != TEF_OK) {
        fprintf(stderr, "sign failed\n");
        return 1;
    }
    if (tefpkg_save_file(pkg, fingerprint) != TEF_OK) {
        fprintf(stderr, "save failed\n");
        return 1;
    }
    tefpkg_close(pkg);
    printf("packed %s -> %s (13 entries, fingerprint=0x%llx)\n", so, out,
           (unsigned long long)fingerprint);
    return 0;
}
