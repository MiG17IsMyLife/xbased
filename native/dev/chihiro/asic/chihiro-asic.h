/*
 * Chihiro Type-3 media board ASIC — the machine around the V850 core
 *
 * Copyright (c) 2026 Réda Chérif-Touil
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */
#ifndef CHIHIRO_ASIC_H
#define CHIHIRO_ASIC_H

#include <stdint.h>
#include <stdbool.h>

bool chihiro_asic_init(const char *path, unsigned dimm_factor);
bool chihiro_asic_load_upload(const uint8_t *image, uint32_t size);
void chihiro_asic_set_running(bool running);
void chihiro_asic_host_mailbox_write(uint32_t offset, uint32_t val);
bool chihiro_asic_mailbox_exchange(const uint32_t *cmd8, uint32_t *msg8);
bool chihiro_asic_tick(uint32_t *msg8);

#endif
