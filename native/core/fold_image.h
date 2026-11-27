/*
 * xbased native core — an extracted Xbox or Chihiro game folder presented
 * as an XDVDFS image, with directory tables built in memory and sectors
 * read from the files themselves.
 */
#ifndef NATIVE_CORE_FOLD_IMAGE_H
#define NATIVE_CORE_FOLD_IMAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Present a folder as a disc using its open default.xbe or Chihiro boot.id.
 * The descriptor identifies the folder in subsequent nc_fold_read calls. */
bool nc_fold_attach(int fd);
/* The folder holding the file open as fd. */
bool nc_fold_fd_dir(int fd, char *out, size_t cap);
void nc_fold_detach(int fd);
/* fd registered as a folder */
bool nc_fold_is(int fd);
/* len bytes of the image at off; zeros past its end. False: a file that
 * can't be read. Any thread. */
bool nc_fold_read(int fd, void *buf, size_t len, uint64_t off);

/* A path the front end may hand over for a game: a folder holding a
 * default.xbe (or that file itself) gives the default.xbe's path in out,
 * true. Anything else (a disc image) false, out untouched. */
bool nc_fold_xbe_path(const char *path, char *out, size_t cap);

/* A folder as a modded Xbox keeps its disc images (#367): the game's image
 * with an ISO attacher's default.xbe beside it, a small XBE with the
 * game's name and title ID that mounts the image as the DVD drive and
 * starts it. path: the folder or its default.xbe. The folder's one image
 * (.iso .xiso .chd, over 1 MiB) in out, true; none, or more than one (a
 * split image's parts), false, out untouched. */
bool nc_fold_attached_image(const char *path, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
