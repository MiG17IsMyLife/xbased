/*
 * Small file helpers for the cabinet's save files (file_util.h).
 */
#include "file_util.h"

#include "native/core/nc.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#endif

void chihiro_mkdir(const char *path)
{
#ifdef _WIN32
    _mkdir(path);
#else
    mkdir(path, 0755);
#endif
}

bool chihiro_file_replace(const char *path, const ChihiroFilePart *parts, int count)
{
    /* The temporary file sits next to the target, so the rename stays on
     * one drive. The ".tmp" ending keeps it out of the card list (cards.c). */
    char tmp[1024 + 32];
    const int n = snprintf(tmp, sizeof(tmp), "%s.%ld.tmp", path, (long)getpid());
    if (n < 0 || (size_t)n >= sizeof(tmp)) {
        return false;
    }
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        nc_log("xbased-chihiro", "can't write %s: %s", tmp, strerror(errno));
        return false;
    }
    bool ok = true;
    for (int i = 0; i < count && ok; i++) {
        ok = fwrite(parts[i].data, 1, parts[i].size, f) == parts[i].size;
    }
    ok = fclose(f) == 0 && ok;
#ifdef _WIN32
    ok = ok && MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING);
#else
    ok = ok && rename(tmp, path) == 0;
#endif
    if (!ok) {
        nc_log("xbased-chihiro", "can't replace %s", path);
        remove(tmp);
        return false;
    }
    return true;
}
