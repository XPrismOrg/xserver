/* SPDX-License-Identifier: MIT OR X11
 *
 * PAL platform module. The values are fixed at build time by
 * os/pal/meson.build.
 */
#include <dix-config.h>

#include "os/pal/pal.h"

const char *
pal_platform_name(void)
{
    return PAL_PLATFORM_NAME;
}

const char *
pal_platform_input_backend(void)
{
    return PAL_INPUT_BACKEND;
}
