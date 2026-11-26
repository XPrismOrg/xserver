/* SPDX-License-Identifier: MIT */
/*
 * XPRISM-COLOR protocol, version 0.3 (wire format).
 *
 * Applications say which colour space their windows' pixels are in; the
 * server's scene compositor converts them for the outputs. See
 * doc/xprism-color-protocol.md and doc/color.md.
 */
#ifndef XPRISM_COLOR_PROTO_H
#define XPRISM_COLOR_PROTO_H

#include <X11/Xmd.h>

#define XPRISM_COLOR_NAME           "XPRISM-COLOR"
#define XPRISM_COLOR_MAJOR          0
#define XPRISM_COLOR_MINOR          3

#define X_XpColorQueryVersion       0
#define X_XpColorSetWindowColorSpace 1
#define X_XpColorGetWindowColorSpace 2
#define X_XpColorClearWindowColorSpace 3
#define X_XpColorGetOutputColorSpace 4  /* 0.2 */
#define X_XpColorSelectOutputInput  5   /* 0.3 */

/* events (0.3) */
#define XpColorOutputColorSpaceNotify 0
#define XpColorNumberEvents         1

/* primaries */
#define XpColorPrimariesCustom      0   /* the xy coordinates sent */
#define XpColorPrimariesSRGB        1   /* = BT.709 */
#define XpColorPrimariesDisplayP3   2
#define XpColorPrimariesBT2020      3
#define XpColorPrimariesLast        XpColorPrimariesBT2020

/* transfer functions */
#define XpColorTransferFnSRGB       0
#define XpColorTransferFnGamma22    1
#define XpColorTransferFnLinear     2
#define XpColorTransferFnPQ         3
#define XpColorTransferFnHLG        4
#define XpColorTransferFnLast       XpColorTransferFnHLG

#define XpColorMaxLuminance         10000       /* cd/m^2 */

typedef struct {
    CARD8 reqType;
    CARD8 colorReqType;
    CARD16 length;
    CARD32 majorVersion;
    CARD32 minorVersion;
} xXpColorQueryVersionReq;
#define sz_xXpColorQueryVersionReq 12

typedef struct {
    BYTE type;                  /* X_Reply */
    BYTE pad1;
    CARD16 sequenceNumber;
    CARD32 length;
    CARD32 majorVersion;
    CARD32 minorVersion;
    CARD32 pad2, pad3, pad4, pad5;
} xXpColorQueryVersionReply;
#define sz_xXpColorQueryVersionReply 32

/*
 * A colour space description.
 *
 * primaries:  XpColorPrimaries*; for Custom, xy holds the CIE 1931
 *             chromaticities of red, green, blue and white as x, y pairs
 *             in millionths; otherwise xy is ignored (zero).
 * transfer:   XpColorTransferFn*.
 * refWhite:   luminance of reference (SDR) white, cd/m^2; 0 = the
 *             transfer function's default (80 for SDR curves, 203 for
 *             PQ and HLG).
 * minLum:     lowest luminance of the content, 1/10000 cd/m^2.
 * maxLum:     highest luminance of the content (mastering display),
 *             cd/m^2; 0 = default (refWhite for SDR curves, 10000 for
 *             PQ, 1000 for HLG). For HLG it is the display peak the
 *             content's OOTF assumes.
 */
typedef struct {
    CARD8 primaries;
    CARD8 transfer;
    CARD16 pad;
    CARD32 xy[8];
    CARD32 refWhite;
    CARD32 minLum;
    CARD32 maxLum;
} xXpColorSpaceDesc;
#define sz_xXpColorSpaceDesc 48

typedef struct {
    CARD8 reqType;
    CARD8 colorReqType;
    CARD16 length;
    CARD32 window;
    xXpColorSpaceDesc desc;
} xXpColorSetWindowColorSpaceReq;
#define sz_xXpColorSetWindowColorSpaceReq 56

typedef struct {
    CARD8 reqType;
    CARD8 colorReqType;
    CARD16 length;
    CARD32 window;
} xXpColorGetWindowColorSpaceReq;
#define sz_xXpColorGetWindowColorSpaceReq 8

/* The window's own description, with defaults filled in and named
   primaries' coordinates given; set = 0 if it has none (then sRGB). */
typedef struct {
    BYTE type;                  /* X_Reply */
    BYTE set;
    CARD16 sequenceNumber;
    CARD32 length;              /* 6 */
    xXpColorSpaceDesc desc;
} xXpColorGetWindowColorSpaceReply;
#define sz_xXpColorGetWindowColorSpaceReply 56

typedef struct {
    CARD8 reqType;
    CARD8 colorReqType;
    CARD16 length;
    CARD32 window;
} xXpColorClearWindowColorSpaceReq;
#define sz_xXpColorClearWindowColorSpaceReq 8

/* 0.2: what a RandR output shows; set = 0 for sRGB. Same reply layout
   as GetWindowColorSpace. */
typedef struct {
    CARD8 reqType;
    CARD8 colorReqType;
    CARD16 length;
    CARD32 output;
} xXpColorGetOutputColorSpaceReq;
#define sz_xXpColorGetOutputColorSpaceReq 8

typedef xXpColorGetWindowColorSpaceReply xXpColorGetOutputColorSpaceReply;

/* 0.3: OutputColorSpaceNotify events, for the outputs of window's
   screen, on or off */
typedef struct {
    CARD8 reqType;
    CARD8 colorReqType;
    CARD16 length;
    CARD32 window;
    BOOL enable;
    CARD8 pad1;
    CARD16 pad2;
} xXpColorSelectOutputInputReq;
#define sz_xXpColorSelectOutputInputReq 12

/* 0.3: what output shows changed; GetOutputColorSpace says what */
typedef struct {
    BYTE type;
    BYTE pad1;
    CARD16 sequenceNumber;
    CARD32 window;              /* as selected */
    CARD32 output;
    CARD32 timestamp;
    CARD32 pad2, pad3, pad4, pad5;
} xXpColorOutputColorSpaceNotifyEvent;
#define sz_xXpColorOutputColorSpaceNotifyEvent 32

#endif /* XPRISM_COLOR_PROTO_H */
