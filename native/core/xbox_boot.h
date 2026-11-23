/*
 * Xbox boot media and XBE selection.
 */
#ifndef NATIVE_CORE_XBOX_BOOT_H
#define NATIVE_CORE_XBOX_BOOT_H

#include "native/core/core.h"

/* The disc probe may adjust the console RAM before the VM is created. */
uint32_t nc_xbox_boot_ram(int disc_fd, const NcBootOptions *boot);

/* Opens the boot media and loads the selected XBE into the initialized VM
 * and kernel. Takes ownership of disc_fd. A mounted disc remains owned by c.
 * On failure, sets the frontend's fatal error. */
bool nc_xbox_boot_xbe(NcCore *c, int disc_fd, const NcBootOptions *boot);

#endif
