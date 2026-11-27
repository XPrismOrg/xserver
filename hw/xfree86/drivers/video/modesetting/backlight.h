/* SPDX-License-Identifier: MIT */
/*
 * XPrism: the backlight of a built-in panel, for extended dynamic range
 * (EDR). The compositor asks for a headroom: the backlight is set to the
 * user's brightness times it, while the compositor dims everything but
 * the brighter content by the same factor.
 */
#ifndef MS_BACKLIGHT_H
#define MS_BACKLIGHT_H

#include "os.h"

typedef struct ms_backlight {
    char *name;                 /* in /sys/class/backlight */
    int max;                    /* max_brightness */
    int user;                   /* the brightness the user set */
    int written;                /* what was last set, -1: nothing */
    double headroom;            /* the compositor's, 1: none */
    Bool suspended;             /* on another VT: the user's brightness */
    Bool failed;                /* cannot be set: no EDR */
    OsTimerPtr timer;           /* notices the user's changes */
} ms_backlight;

/* The backlight of the panel driven by the DRM device fd, or NULL */
ms_backlight *ms_backlight_find(int fd);
void ms_backlight_free(ms_backlight *bl);

/* The brightness now, -1 if it cannot be read */
int ms_backlight_read(ms_backlight *bl);

/* The user's brightness times headroom (while not suspended); FALSE if it
   could not be set */
Bool ms_backlight_apply(ms_backlight *bl, double headroom);

/* Back to the user's brightness while on another VT, and again */
void ms_backlight_suspend(ms_backlight *bl, Bool suspend);

/* How much brighter than the user's brightness it can go */
double ms_backlight_headroom(const ms_backlight *bl);

/* The user changed the brightness since it was last looked at (someone
   else wrote it): take it as theirs. TRUE if it changed. */
Bool ms_backlight_update_user(ms_backlight *bl);

#endif
