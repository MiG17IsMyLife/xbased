/*
 * Chihiro direct boot from an extracted game folder.
 *
 * boot.id layout reference: Tovarichtch/xemu, chihiro_bootid_executable
 * and chihiro_game_name_of in hw/xbox/chihiro/chihiro.c (98fbd75).
 */
#include "native/core/chihiro_boot.h"
#include "native/core/fold_image.h"

#include <stdint.h>

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef O_BINARY
#define O_BINARY 0
#endif

/* boot.id directly in the folder dir */
static bool boot_in_dir(const char *path, char *out, size_t cap)
{
    DIR *dir = opendir(path);
    if (!dir) {
        return false;
    }
    bool found = false;
    for (struct dirent *entry; (entry = readdir(dir));) {
        if (strcasecmp(entry->d_name, "boot.id")) {
            continue;
        }
        char file[1024];
        const int len = snprintf(file, sizeof(file), "%s/%s", path, entry->d_name);
        struct stat st;
        if (len > 0 && (size_t)len < sizeof(file) && (size_t)len < cap &&
            !stat(file, &st) && S_ISREG(st.st_mode)) {
            const int fd = open(file, O_RDONLY | O_CLOEXEC | O_BINARY);
            if (fd >= 0) {
                char magic[4];
                found = pread(fd, magic, sizeof(magic), 0) == sizeof(magic) &&
                        !memcmp(magic, NC_CHIHIRO_BOOT_MAGIC, sizeof(magic));
                close(fd);
            }
            if (found) {
                memcpy(out, file, (size_t)len + 1);
                break;
            }
        }
    }
    closedir(dir);
    return found;
}

static bool boot_executable(const char field[32], char out[32]);

/* xbe's path below root, with backslashes; false if it isn't below it */
static bool relative_to(const char *xbe, const char *root, char *out, size_t cap)
{
    const size_t n = strlen(root);
    if (strncasecmp(xbe, root, n) || (xbe[n] != '/' && xbe[n] != '\\')) {
        return false;
    }
    snprintf(out, cap, "%s", xbe + n + 1);
    for (char *c = out; *c; c++) {
        *c = *c == '/' ? '\\' : *c;
    }
    return true;
}

/* An XBE inside an extracted game: the boot.id up to 3 folders above it
 * that names it as the game's or the test program's. */
static bool boot_of_xbe(const char *xbe, char *out, size_t cap, bool *test)
{
    char root[1024];
    snprintf(root, sizeof(root), "%s", xbe);
    for (int up = 0; up < 3; up++) {
        char *a = strrchr(root, '/'), *b = strrchr(root, '\\');
        char *cut = a > b ? a : b;
        if (!cut) {
            return false;
        }
        *cut = 0;
        if (!boot_in_dir(root, out, cap)) {
            continue;
        }
        const int fd = open(out, O_RDONLY | O_CLOEXEC | O_BINARY);
        NcChihiroBoot game;
        const bool ok = fd >= 0 && !nc_chihiro_boot_probe(fd, false, &game) && game.title;
        if (fd >= 0) {
            close(fd);
        }
        char rel[1024];
        if (!ok || !relative_to(xbe, root, rel, sizeof(rel))) {
            return false;
        }
        if (!strcasecmp(rel, game.game_executable)) {
            *test = false;
            return true;
        }
        if (game.test_executable[0] && !strcasecmp(rel, game.test_executable)) {
            *test = true;
            return true;
        }
        return false;
    }
    return false;
}

unsigned nc_chihiro_boot_tag(int boot_fd, const NcChihiroBoot *game)
{
    char dir[1024], xbe[1100];
    if (!nc_fold_fd_dir(boot_fd, dir, sizeof(dir))) {
        return 0;
    }
    snprintf(xbe, sizeof(xbe), "%s/%s", dir, game->game_executable);
    for (char *c = xbe + strlen(dir); *c; c++) {
        *c = *c == '\\' ? '/' : *c;
    }
    const int fd = open(xbe, O_RDONLY | O_CLOEXEC | O_BINARY);
    if (fd < 0) {
        return 0;
    }
    uint8_t head[4096];
    const ssize_t n = pread(fd, head, sizeof(head), 0);
    close(fd);
    unsigned h = 2166136261u;
    for (ssize_t i = 0; i < n; i++) {
        h = (h ^ head[i]) * 16777619u;
    }
    return n > 0 ? h : 0;
}

bool nc_chihiro_boot_path(const char *path, char *out, size_t cap, bool *test)
{
    if (test) {
        *test = false;
    }
    struct stat st;
    if (!path || stat(path, &st)) {
        return false;
    }
    if (S_ISDIR(st.st_mode)) {
        return boot_in_dir(path, out, cap);
    }
    const char *name = path + strlen(path);
    while (name > path && name[-1] != '/' && name[-1] != '\\') {
        name--;
    }
    const size_t n = strlen(path);
    if (n > 4 && !strcasecmp(path + n - 4, ".xbe")) {
        bool t = false;
        const bool found = boot_of_xbe(path, out, cap, &t);
        if (test) {
            *test = found && t;
        }
        return found;
    }
    if (!S_ISREG(st.st_mode) || strcasecmp(name, "boot.id") || n >= cap) {
        return false;
    }
    const int fd = open(path, O_RDONLY | O_CLOEXEC | O_BINARY);
    if (fd < 0) {
        return false;
    }
    char magic[4];
    const bool found = pread(fd, magic, sizeof(magic), 0) == sizeof(magic) &&
                       !memcmp(magic, NC_CHIHIRO_BOOT_MAGIC, sizeof(magic));
    close(fd);
    if (found) {
        memcpy(out, path, n + 1);
    }
    return found;
}

bool nc_chihiro_xbe_path(const char *path, char *out, size_t cap, char *root, size_t root_cap, char game_id[5])
{
    char boot[1024];
    if (!nc_chihiro_boot_path(path, boot, sizeof(boot), NULL)) {
        return false;
    }
    const int fd = open(boot, O_RDONLY | O_CLOEXEC | O_BINARY);
    NcChihiroBoot game;
    const bool ok = fd >= 0 && !nc_chihiro_boot_probe(fd, false, &game) && game.title;
    if (fd >= 0) {
        close(fd);
    }
    if (!ok) {
        return false;
    }
    const char *name = boot + strlen(boot);
    while (name > boot && name[-1] != '/' && name[-1] != '\\') {
        name--;
    }
    const size_t prefix = (size_t)(name - boot);
    const size_t root_len = prefix ? prefix - 1 : 1;
    if (root_len >= root_cap || prefix + strlen(game.game_executable) >= cap) {
        return false;
    }
    snprintf(root, root_cap, "%.*s", (int)root_len, prefix ? boot : ".");
    snprintf(out, cap, "%.*s%s", (int)prefix, boot, game.game_executable);
    for (char *c = out + prefix; *c; c++) {
        *c = *c == '\\' ? '/' : *c;
    }
    memcpy(game_id, game.game_id, sizeof(game.game_id));
    return true;
}

static bool boot_executable(const char field[32], char out[32])
{
    const char *end = memchr(field, 0, 32);
    if (!end) {
        return false;
    }
    const char *name = field + strspn(field, "\\/");
    const size_t len = (size_t)(end - name);
    if (len <= 4 || strcasecmp(end - 4, ".xbe")) {
        return false;
    }
    for (size_t i = 0, start = 0; i <= len; i++) {
        const char c = name[i];
        if (c == '\\' || c == '/' || !c) {
            const size_t size = i - start;
            if (!size || (size == 1 && name[start] == '.') ||
                (size == 2 && name[start] == '.' && name[start + 1] == '.')) {
                return false;
            }
            out[i] = c ? '\\' : 0;
            start = i + 1;
        } else if ((unsigned char)c < 0x20 || strchr(":*?\"<>|", c)) {
            return false;
        } else {
            out[i] = c;
        }
    }
    return true;
}

const char *nc_chihiro_boot_probe(int fd, bool test, NcChihiroBoot *game)
{
    enum { MEDIA_TYPE = 0x20, GAME_ID = 0x30, EXECUTABLE = 0xA0, TEST_EXECUTABLE = 0xC0 };
    char data[TEST_EXECUTABLE + sizeof(game->executable)];
    memset(game, 0, sizeof(*game));
    if (fd < 0) {
        return NULL;
    }
    const ssize_t len = pread(fd, data, sizeof(data), 0);
    if (len < 4) {
        return NULL;
    }
    if (!memcmp(data, "FATX", 4)) {
        return "For Chihiro, open the extracted game folder or its boot.id instead of a FATX image.";
    }
    if (memcmp(data, NC_CHIHIRO_BOOT_MAGIC, 4)) {
        return NULL;
    }
    if (len < EXECUTABLE + (ssize_t)sizeof(game->executable) || memcmp(data + MEDIA_TYPE, "XBAM", 4)) {
        return "The Chihiro boot.id is damaged or has an invalid executable path.";
    }
    if (!boot_executable(data + EXECUTABLE, game->game_executable)) {
        return "The Chihiro boot.id has no valid game executable.";
    }
    if (len < TEST_EXECUTABLE + (ssize_t)sizeof(game->executable) ||
        !boot_executable(data + TEST_EXECUTABLE, game->test_executable)) {
        if (test) {
            return "The Chihiro boot.id has no valid test executable.";
        }
        game->test_executable[0] = 0;
    }
    memcpy(game->executable, test ? game->test_executable : game->game_executable, sizeof(game->executable));
    memcpy(game->game_id, data + GAME_ID, 4);
    if (!memcmp(data + GAME_ID, "SBHQ", 4)) {
        game->title = "Wangan Midnight Maximum Tune";
    } else if (!memcmp(data + GAME_ID, "SBKD", 4)) {
        game->title = "Wangan Midnight Maximum Tune 2";
    } else {
        return "This Chihiro title is outside the current Maximum Tune 1 and 2 support.";
    }
    return NULL;
}
