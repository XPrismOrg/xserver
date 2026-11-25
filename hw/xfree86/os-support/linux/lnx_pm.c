#include <xorg-config.h>

#include <X11/X.h>

#include "os/log_priv.h"

#include "os.h"
#include "xf86_priv.h"
#include "xf86Priv.h"
#include "xf86_os_support.h"
#include "xf86_OSproc.h"

#ifdef HAVE_ACPI
extern PMClose lnxACPIOpen(void);
#endif

PMClose
xf86OSPMOpen(void)
{
    PMClose ret = NULL;

#ifdef HAVE_ACPI
    if (!xf86acpiDisableFlag) {
        ret = lnxACPIOpen();
        if (ret)
            return ret;
    }
#endif

    return ret;
}

