/* SPDX-License-Identifier: MIT OR X11 */
/*
 * Binary-compatibility stubs for the removed DGA extension.
 *
 * DGA (direct framebuffer access for clients) has been removed. Drivers
 * built against the current SDK no longer see these functions, but
 * binary drivers built against an older SDK may still import them.
 * Keep the symbols so such drivers still load, and report that DGA is
 * unavailable.
 */
#include <xorg-config.h>

#include "xf86.h"
#include "xf86str.h"
#include "xf86Crtc.h"

_X_EXPORT Bool DGAInit(ScreenPtr pScreen, DGAFunctionPtr funcs,
                       DGAModePtr modes, int num);
_X_EXPORT Bool DGAReInitModes(ScreenPtr pScreen, DGAModePtr modes, int num);
_X_EXPORT int xf86SetDGAMode(ScrnInfoPtr pScrn, int num, DGADevicePtr devRet);
_X_EXPORT Bool xf86DiDGAInit(ScreenPtr pScreen, unsigned long dga_address);
_X_EXPORT Bool xf86DiDGAReInit(ScreenPtr pScreen);

Bool
DGAInit(ScreenPtr pScreen, DGAFunctionPtr funcs, DGAModePtr modes, int num)
{
    return FALSE;
}

Bool
DGAReInitModes(ScreenPtr pScreen, DGAModePtr modes, int num)
{
    return FALSE;
}

int
xf86SetDGAMode(ScrnInfoPtr pScrn, int num, DGADevicePtr devRet)
{
    return BadImplementation;
}

Bool
xf86DiDGAInit(ScreenPtr pScreen, unsigned long dga_address)
{
    return FALSE;
}

Bool
xf86DiDGAReInit(ScreenPtr pScreen)
{
    return FALSE;
}
