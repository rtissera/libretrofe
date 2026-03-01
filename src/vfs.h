// SPDX-License-Identifier: MIT
#ifndef LIBRA_VFS_H
#define LIBRA_VFS_H

#include "libretro.h"

/* VFS API version we implement (v1=basic, v2=+truncate) */
#define LIBRA_VFS_VERSION 2

struct retro_vfs_interface *libra_vfs_get_interface(void);

#endif /* LIBRA_VFS_H */
