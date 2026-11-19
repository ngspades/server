// Copyright (c) 2026 ngspades contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <Util/DataDir.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif

static char folder[1024];

static int _make(const char* path)
{
    struct stat st;
    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode) ? 0 : -1;
    }
#ifdef _WIN32
    return _mkdir(path) == 0 || errno == EEXIST ? 0 : -1;
#else
    return mkdir(path, 0755) == 0 || errno == EEXIST ? 0 : -1;
#endif
}

int data_dir_set(const char* dir)
{
    folder[0] = '\0';
    if (dir == NULL || *dir == '\0') {
        return 0;
    }
    size_t n = strlen(dir);
    if (n + 2 > sizeof folder) {
        return -1;
    }
    /* each missing parent too: the client hands over a folder inside its
       own user folder, which may not exist yet on a first start */
    char path[sizeof folder];
    snprintf(path, sizeof path, "%s", dir);
    for (char* p = path + 1; *p != '\0'; p++) {
        /* Windows' folders come with backslashes */
        if (*p == '/' || *p == '\\') {
            char sep = *p;
            *p       = '\0';
            _make(path);
            *p = sep;
        }
    }
    if (_make(path) != 0) {
        return -1;
    }
    snprintf(folder, sizeof folder, "%s%s", dir, dir[n - 1] == '/' ? "" : "/");
    return 0;
}

const char* data_dir(void)
{
    return folder;
}

const char* data_path(char* out, size_t size, const char* name)
{
    snprintf(out, size, "%s%s", folder, name);
    return out;
}

const char* data_bans_file(void)
{
    static char bans[sizeof folder + 16];
    return data_path(bans, sizeof bans, "Bans.json");
}
