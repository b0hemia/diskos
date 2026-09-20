/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#include "fwcaps.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Verified RE data points (see RE_CATALOGUE / the V228 delta):
 *   V2.09  gain set = tag 0645   (no 0649 handler)
 *   V2.28  gain set = tag 0649   (the 0645 handler is a NULL pointer -> silent no-op)
 *   V2.40  gain set = tag 0649   (firmware audit 2026-09-05: on the v2.40 player, 0649 resolves
 *                                 0x413930 -> callback 0x4e8654 = the gain setter; 0645 is a NULL
 *                                 callback. Same tag as V2.28.)
 * We map only versions with RE data and fail closed on anything else (unknown/unreadable version ->
 * send no gain command) rather than guessing a tag that might hit an unrelated handler. Each new
 * firmware is gain-tested on-device before publish; add its verified {version, tag} entry here then.
 * NOTE: the v2.40 entry is RE-verified but the Low/High acoustic effect still wants an on-device check. */
static const struct { int ver; const char *tag; } GAIN_MAP[] = {
    { 209, "0645" },
    { 228, "0649" },
    { 240, "0649" },
};

static int parse_os_ver(void){
    FILE *f = fopen("/etc/product_version/version.in", "r");
    if(!f) return 0;
    char line[128];
    int ver = 0;
    while(fgets(line, sizeof line, f)){
        size_t ll = strlen(line);
        if(!(ll && line[ll-1]=='\n') && ll == sizeof(line)-1){   /* overlong physical line -> drain + skip, so a
                                                                  * padded/truncated value can't be read as a real
                                                                  * MAIN_OS_VER and enable a wrong-firmware capability */
            int c; while((c=fgetc(f))!=EOF && c!='\n'){}
            continue;
        }
        char *p = line;
        while(*p == ' ' || *p == '\t') p++;                 /* tolerate leading whitespace */
        if(strncmp(p, "MAIN_OS_VER=", 12) == 0){
            char *num = p + 12, *end = NULL;
            long n = strtol(num, &end, 10);
            if(end == num){ ver = 0; break; }               /* no digits -> unknown */
            while(*end==' '||*end=='\t'||*end=='\r'||*end=='\n') end++;
            ver = (*end == '\0' && n > 0 && n < 100000) ? (int)n : 0;  /* reject trailing garbage (e.g. "228junk") */
            break;
        }
    }
    fclose(f);
    return ver > 0 ? ver : 0;
}

int fw_os_ver(void){
    static int cached = -1;                                 /* -1 = not read yet */
    if(cached < 0) cached = parse_os_ver();
    return cached;
}

const char *fw_gain_tag(void){
    int v = fw_os_ver();
    for(unsigned i = 0; i < sizeof GAIN_MAP / sizeof GAIN_MAP[0]; i++)
        if(GAIN_MAP[i].ver == v) return GAIN_MAP[i].tag;
    return NULL;   /* unverified/unreadable firmware -> caller must not send a gain command */
}
