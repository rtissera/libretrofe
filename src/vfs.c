// SPDX-License-Identifier: MIT
#include "vfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>  /* ftruncate, fileno */
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>

struct retro_vfs_file_handle {
    FILE *fp;
    char *path;
};

/* ---- v1 functions -------------------------------------------------------- */

static const char *vfs_get_path(struct retro_vfs_file_handle *stream)
{
    return stream ? stream->path : NULL;
}

static struct retro_vfs_file_handle *vfs_open(const char *path,
                                               unsigned mode, unsigned hints)
{
    (void)hints;
    if (!path)
        return NULL;

    const char *fmode;
    bool read  = (mode & RETRO_VFS_FILE_ACCESS_READ)  != 0;
    bool write = (mode & RETRO_VFS_FILE_ACCESS_WRITE) != 0;
    bool update = (mode & RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING) != 0;

    if (read && write)
        fmode = update ? "r+b" : "w+b";
    else if (write)
        fmode = update ? "r+b" : "wb";
    else
        fmode = "rb";

    FILE *fp = fopen(path, fmode);
    if (!fp)
        return NULL;

    struct retro_vfs_file_handle *h = malloc(sizeof(*h));
    if (!h) { fclose(fp); return NULL; }

    h->fp   = fp;
    h->path = strdup(path);
    return h;
}

static int vfs_close(struct retro_vfs_file_handle *stream)
{
    if (!stream)
        return -1;
    int r = fclose(stream->fp);
    free(stream->path);
    free(stream);
    return r ? -1 : 0;
}

static int64_t vfs_size(struct retro_vfs_file_handle *stream)
{
    if (!stream)
        return -1;
    long cur = ftell(stream->fp);
    if (cur < 0 || fseek(stream->fp, 0, SEEK_END) != 0)
        return -1;
    long sz = ftell(stream->fp);
    fseek(stream->fp, cur, SEEK_SET);
    return (int64_t)sz;
}

static int64_t vfs_tell(struct retro_vfs_file_handle *stream)
{
    if (!stream)
        return -1;
    return (int64_t)ftell(stream->fp);
}

static int64_t vfs_seek(struct retro_vfs_file_handle *stream,
                         int64_t offset, int seek_position)
{
    if (!stream)
        return -1;
    int whence;
    switch (seek_position) {
        case RETRO_VFS_SEEK_POSITION_START:   whence = SEEK_SET; break;
        case RETRO_VFS_SEEK_POSITION_CURRENT: whence = SEEK_CUR; break;
        case RETRO_VFS_SEEK_POSITION_END:     whence = SEEK_END; break;
        default: return -1;
    }
    if (fseek(stream->fp, (long)offset, whence) != 0)
        return -1;
    return (int64_t)ftell(stream->fp);
}

static int64_t vfs_read(struct retro_vfs_file_handle *stream,
                         void *s, uint64_t len)
{
    if (!stream || !s)
        return -1;
    return (int64_t)fread(s, 1, (size_t)len, stream->fp);
}

static int64_t vfs_write(struct retro_vfs_file_handle *stream,
                          const void *s, uint64_t len)
{
    if (!stream || !s)
        return -1;
    return (int64_t)fwrite(s, 1, (size_t)len, stream->fp);
}

static int vfs_flush(struct retro_vfs_file_handle *stream)
{
    if (!stream)
        return -1;
    return fflush(stream->fp) ? -1 : 0;
}

static int vfs_remove(const char *path)
{
    if (!path)
        return -1;
    return remove(path) ? -1 : 0;
}

static int vfs_rename(const char *old_path, const char *new_path)
{
    if (!old_path || !new_path)
        return -1;
    return rename(old_path, new_path) ? -1 : 0;
}

/* ---- v2 functions -------------------------------------------------------- */

static int64_t vfs_truncate(struct retro_vfs_file_handle *stream, int64_t length)
{
    if (!stream || length < 0)
        return -1;
    return ftruncate(fileno(stream->fp), (off_t)length) ? -1 : 0;
}

/* ---- v3 functions -------------------------------------------------------- */

struct retro_vfs_dir_handle {
    DIR        *dir;
    char       *path;
    struct dirent *entry;     /* current entry from readdir() */
    bool        include_hidden;
};

static int vfs_stat(const char *path, int32_t *size)
{
    if (!path)
        return 0;

    struct stat st;
    if (stat(path, &st) != 0)
        return 0;

    int flags = RETRO_VFS_STAT_IS_VALID;
    if (S_ISDIR(st.st_mode))
        flags |= RETRO_VFS_STAT_IS_DIRECTORY;
    if (S_ISCHR(st.st_mode))
        flags |= RETRO_VFS_STAT_IS_CHARACTER_SPECIAL;

    if (size)
        *size = (int32_t)st.st_size;

    return flags;
}

static int vfs_mkdir(const char *dir)
{
    if (!dir)
        return -1;
    if (mkdir(dir, 0755) == 0)
        return 0;
    if (errno == EEXIST)
        return -2;
    return -1;
}

static struct retro_vfs_dir_handle *vfs_opendir(const char *dir,
                                                 bool include_hidden)
{
    if (!dir)
        return NULL;

    DIR *d = opendir(dir);
    if (!d)
        return NULL;

    struct retro_vfs_dir_handle *h = calloc(1, sizeof(*h));
    if (!h) { closedir(d); return NULL; }

    h->dir            = d;
    h->path           = strdup(dir);
    h->entry          = NULL;
    h->include_hidden = include_hidden;
    return h;
}

static bool vfs_readdir(struct retro_vfs_dir_handle *dirstream)
{
    if (!dirstream || !dirstream->dir)
        return false;

    for (;;) {
        dirstream->entry = readdir(dirstream->dir);
        if (!dirstream->entry)
            return false;

        const char *name = dirstream->entry->d_name;

        /* Always skip . and .. */
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;

        /* Skip hidden files (starting with '.') unless include_hidden */
        if (!dirstream->include_hidden && name[0] == '.')
            continue;

        return true;
    }
}

static const char *vfs_dirent_get_name(struct retro_vfs_dir_handle *dirstream)
{
    if (!dirstream || !dirstream->entry)
        return NULL;
    return dirstream->entry->d_name;
}

static bool vfs_dirent_is_dir(struct retro_vfs_dir_handle *dirstream)
{
    if (!dirstream || !dirstream->entry)
        return false;

#ifdef _DIRENT_HAVE_D_TYPE
    if (dirstream->entry->d_type != DT_UNKNOWN)
        return dirstream->entry->d_type == DT_DIR;
#endif

    /* Fallback: stat the entry */
    char full[4096];
    snprintf(full, sizeof(full), "%s/%s", dirstream->path,
             dirstream->entry->d_name);
    struct stat st;
    if (stat(full, &st) != 0)
        return false;
    return S_ISDIR(st.st_mode);
}

static int vfs_closedir(struct retro_vfs_dir_handle *dirstream)
{
    if (!dirstream)
        return -1;
    int r = closedir(dirstream->dir);
    free(dirstream->path);
    free(dirstream);
    return r ? -1 : 0;
}

/* ---- Interface table ----------------------------------------------------- */

static struct retro_vfs_interface s_vfs = {
    /* v1 */
    .get_path  = vfs_get_path,
    .open      = vfs_open,
    .close     = vfs_close,
    .size      = vfs_size,
    .tell      = vfs_tell,
    .seek      = vfs_seek,
    .read      = vfs_read,
    .write     = vfs_write,
    .flush     = vfs_flush,
    .remove    = vfs_remove,
    .rename    = vfs_rename,
    /* v2 */
    .truncate  = vfs_truncate,
    /* v3 */
    .stat           = vfs_stat,
    .mkdir          = vfs_mkdir,
    .opendir        = vfs_opendir,
    .readdir        = vfs_readdir,
    .dirent_get_name = vfs_dirent_get_name,
    .dirent_is_dir  = vfs_dirent_is_dir,
    .closedir       = vfs_closedir,
};

struct retro_vfs_interface *libra_vfs_get_interface(void)
{
    return &s_vfs;
}
