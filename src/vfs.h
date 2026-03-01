// SPDX-License-Identifier: MIT
#ifndef LIBRA_VFS_H
#define LIBRA_VFS_H

#include "libretro.h"

/* VFS API version we implement (v1=basic, v2=+truncate, v3=+directory ops) */
#define LIBRA_VFS_VERSION 3

struct retro_vfs_interface *libra_vfs_get_interface(void);

#endif /* LIBRA_VFS_H */
