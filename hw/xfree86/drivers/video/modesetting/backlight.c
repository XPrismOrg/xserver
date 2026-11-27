/* SPDX-License-Identifier: MIT */
/*
 * XPrism: the backlight of a built-in panel, for EDR (backlight.h).
 *
 * Backlights are found in /sys/class/backlight. Like logind and the
 * desktops, the choice is a "firmware" one (ACPI, WMI), else "platform",
 * else a "raw" one belonging to this GPU. Where there is no such
 * directory, there is no backlight and no EDR.
 *
 * The brightness is written to sysfs where the server may (running as
 * root), else through the session (logind's SetBrightness).
 */
#include "dix-config.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifdef HAVE_SYS_SYSMACROS_H
#include <sys/sysmacros.h>
#endif
#include <unistd.h>

#include "xf86.h"
#include "../../../os-support/linux/systemd-logind.h"

#include "backlight.h"

#define BACKLIGHT_DIR "/sys/class/backlight"

static int
read_int(const char *name, const char *file)
{
    char path[PATH_MAX], buf[32];
    int fd, n;

    snprintf(path, sizeof(path), BACKLIGHT_DIR "/%s/%s", name, file);
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return -1;
    buf[n] = '\0';
    return atoi(buf);
}

/* A word of up to 31 characters; buf holds 32 */
static Bool
read_word(const char *name, const char *file, char buf[32])
{
    char path[PATH_MAX];
    FILE *f;
    Bool ok;

    snprintf(path, sizeof(path), BACKLIGHT_DIR "/%s/%s", name, file);
    f = fopen(path, "re");
    if (!f)
        return FALSE;
    ok = fscanf(f, "%31s", buf) == 1;
    fclose(f);
    return ok;
}

/* Whether backlight name's device is the GPU's (or inside it) */
static Bool
belongs_to(const char *name, const char *gpu)
{
    char path[PATH_MAX], real[PATH_MAX];

    snprintf(path, sizeof(path), BACKLIGHT_DIR "/%s/device", name);
    if (!gpu || !realpath(path, real))
        return FALSE;
    return !strncmp(real, gpu, strlen(gpu));
}

ms_backlight *
ms_backlight_find(int fd)
{
    static const char *const types[] = { "firmware", "platform", "raw" };
    char gpu_path[PATH_MAX], gpu[PATH_MAX], best[256] = "";
    int best_rank = 3;
    struct stat st;
    DIR *dir;
    struct dirent *e;
    Bool have_gpu = FALSE;

    if (fstat(fd, &st) == 0 && S_ISCHR(st.st_mode)) {
        snprintf(gpu_path, sizeof(gpu_path), "/sys/dev/char/%u:%u/device",
                 major(st.st_rdev), minor(st.st_rdev));
        have_gpu = realpath(gpu_path, gpu) != NULL;
    }
    dir = opendir(BACKLIGHT_DIR);
    if (!dir)
        return NULL;
    while ((e = readdir(dir))) {
        char type[32];
        if (e->d_name[0] == '.' || strlen(e->d_name) >= sizeof(best) ||
            !read_word(e->d_name, "type", type) ||
            read_int(e->d_name, "max_brightness") <= 0)
            continue;
        for (int rank = 0; rank < best_rank; rank++) {
            if (strcmp(type, types[rank]))
                continue;
            /* a raw one is the GPU's own: only this GPU's */
            if (rank == 2 && !(have_gpu && belongs_to(e->d_name, gpu)))
                break;
            best_rank = rank;
            strcpy(best, e->d_name);
            break;
        }
    }
    closedir(dir);
    if (!best[0])
        return NULL;

    ms_backlight *bl = calloc(1, sizeof(*bl));
    if (!bl || !(bl->name = strdup(best))) {
        free(bl);
        return NULL;
    }
    bl->max = read_int(best, "max_brightness");
    bl->user = read_int(best, "brightness");
    bl->written = -1;
    bl->headroom = 1;
    return bl;
}

void
ms_backlight_free(ms_backlight *bl)
{
    if (!bl)
        return;
    TimerFree(bl->timer);
    free(bl->name);
    free(bl);
}

int
ms_backlight_read(ms_backlight *bl)
{
    return read_int(bl->name, "brightness");
}

static Bool
backlight_write(ms_backlight *bl, int value)
{
    char path[PATH_MAX], buf[16];
    int fd, n, len;

    if (value == bl->written)
        return TRUE;
    snprintf(path, sizeof(path), BACKLIGHT_DIR "/%s/brightness", bl->name);
    len = snprintf(buf, sizeof(buf), "%d\n", value);
    fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd >= 0) {
        n = write(fd, buf, len);
        close(fd);
        if (n == len) {
            bl->written = value;
            return TRUE;
        }
    }
    if (systemd_logind_set_brightness(bl->name, value) == 0) {
        bl->written = value;
        return TRUE;
    }
    return FALSE;
}

double
ms_backlight_headroom(const ms_backlight *bl)
{
    if (bl->failed || bl->user <= 0 || bl->max <= bl->user)
        return 1;
    return (double) bl->max / bl->user;
}

Bool
ms_backlight_apply(ms_backlight *bl, double headroom)
{
    int value;

    bl->headroom = headroom > 1 ? headroom : 1;
    if (bl->suspended || bl->user < 0)
        return TRUE;
    value = (int) lrint(bl->user * bl->headroom);
    if (value > bl->max)
        value = bl->max;
    if (bl->headroom == 1)
        value = bl->user;
    if (!backlight_write(bl, value)) {
        bl->failed = TRUE;
        return FALSE;
    }
    return TRUE;
}

void
ms_backlight_suspend(ms_backlight *bl, Bool suspend)
{
    if (bl->suspended == suspend)
        return;
    if (suspend) {
        if (bl->headroom > 1)
            backlight_write(bl, bl->user);
        bl->suspended = TRUE;
        bl->written = -1;       /* others may change it meanwhile */
    } else {
        bl->suspended = FALSE;
        /* the user may have changed it on the other VT */
        int now = ms_backlight_read(bl);
        if (now >= 0)
            bl->user = now;
        ms_backlight_apply(bl, bl->headroom);
    }
}

Bool
ms_backlight_update_user(ms_backlight *bl)
{
    int now;

    if (bl->suspended || (now = ms_backlight_read(bl)) < 0)
        return FALSE;
    if (bl->written < 0) {
        /* nothing written yet: what is there is the user's */
        if (now == bl->user)
            return FALSE;
        bl->user = now;
        return TRUE;
    }
    if (now == bl->written)
        return FALSE;
    /* changed by someone else while raised: theirs, without the raise */
    bl->user = (int) lrint(now / bl->headroom);
    bl->written = now;
    return TRUE;
}
