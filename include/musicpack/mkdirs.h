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
#ifndef MP_MKDIRS_H
#define MP_MKDIRS_H
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
static int mp_mkdirs(const char* path){
    char tmp[800]; snprintf(tmp,sizeof(tmp),"%s",path);
    size_t len=strlen(tmp);
    if(len==0) return -1;
    for(size_t i=1;i<len;i++){
        if(tmp[i]=='/'){ tmp[i]=0; mkdir(tmp,0777); tmp[i]='/'; }
    }
    mkdir(tmp,0777);
    return 0;
}
static void mp_ensure_files(const char * dir){
    mp_mkdirs(dir);
}
#endif
