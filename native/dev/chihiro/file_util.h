/*
 * Small file helpers for the cabinet's save files (cards, backup memory).
 */
#ifndef NATIVE_DEV_CHIHIRO_FILE_UTIL_H
#define NATIVE_DEV_CHIHIRO_FILE_UTIL_H

#include <stdbool.h>
#include <stddef.h>

/* One piece of a file to write. */
typedef struct ChihiroFilePart {
    const void *data;
    size_t size;
} ChihiroFilePart;

/* Writes the parts, in order, as the whole new content of path.
 * The data goes to a temporary file first, which is then renamed over
 * path, so a crash never leaves a half-written file behind. */
bool chihiro_file_replace(const char *path, const ChihiroFilePart *parts, int count);

/* Creates a folder; does nothing if it already exists. */
void chihiro_mkdir(const char *path);

#endif
