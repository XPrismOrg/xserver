/* SPDX-License-Identifier: MIT */
/*
 * XPRISM-SCENE protocol, version 0.2 (wire format).
 *
 * The window manager describes how the server's scene compositor shows
 * each top-level window; the server does the pixels (AD-1). See
 * doc/xprism-scene-protocol.md.
 */
#ifndef XPRISM_SCENE_PROTO_H
#define XPRISM_SCENE_PROTO_H

#include <X11/Xmd.h>

#define XPRISM_SCENE_NAME           "XPRISM-SCENE"
#define XPRISM_SCENE_MAJOR          0
#define XPRISM_SCENE_MINOR          2

#define X_XpSceneQueryVersion       0
#define X_XpSceneSetNodeAttributes  1
#define X_XpSceneGetNodeAttributes  2
#define X_XpSceneClearNodeAttributes 3
#define X_XpSceneSetNodeDecoration  4   /* 0.2 */
#define X_XpSceneGetNodeDecoration  5   /* 0.2 */

/* attribute mask bits */
#define XpSceneNodeOpacity          (1u << 0)
#define XpSceneNodeTransform        (1u << 1)
#define XpSceneNodeCornerRadius     (1u << 2)   /* 0.2 */
#define XpSceneNodeShadow           (1u << 3)   /* 0.2 */
#define XpSceneNodeAttributes       (XpSceneNodeOpacity | XpSceneNodeTransform)
#define XpSceneNodeDecoration       (XpSceneNodeCornerRadius | XpSceneNodeShadow)
#define XpSceneNodeAll              (XpSceneNodeAttributes | XpSceneNodeDecoration)

#define XpSceneMaxCornerRadius      1024
#define XpSceneMaxShadowBlur        128

typedef struct {
    CARD8 reqType;
    CARD8 sceneReqType;
    CARD16 length;
    CARD32 majorVersion;
    CARD32 minorVersion;
} xXpSceneQueryVersionReq;
#define sz_xXpSceneQueryVersionReq 12

typedef struct {
    BYTE type;                  /* X_Reply */
    BYTE pad1;
    CARD16 sequenceNumber;
    CARD32 length;
    CARD32 majorVersion;
    CARD32 minorVersion;
    CARD32 pad2, pad3, pad4, pad5;
} xXpSceneQueryVersionReply;
#define sz_xXpSceneQueryVersionReply 32

/*
 * opacity:   0 (invisible) .. 0xffffffff (opaque), as _NET_WM_WINDOW_OPACITY
 * transform: 16.16 fixed-point 3x3 matrix, row-major, mapping a point of
 *            the window (origin: outer top-left corner, border included)
 *            to where it is shown, relative to the same origin.
 */
typedef struct {
    CARD8 reqType;
    CARD8 sceneReqType;
    CARD16 length;
    CARD32 window;
    CARD32 mask;
    CARD32 opacity;
    INT32 transform[9];
} xXpSceneSetNodeAttributesReq;
#define sz_xXpSceneSetNodeAttributesReq 52

typedef struct {
    CARD8 reqType;
    CARD8 sceneReqType;
    CARD16 length;
    CARD32 window;
} xXpSceneGetNodeAttributesReq;
#define sz_xXpSceneGetNodeAttributesReq 8

typedef struct {
    BYTE type;                  /* X_Reply */
    BYTE pad1;
    CARD16 sequenceNumber;
    CARD32 length;              /* 5 */
    CARD32 mask;                /* attributes that are set */
    CARD32 opacity;
    INT32 transform[9];
} xXpSceneGetNodeAttributesReply;
#define sz_xXpSceneGetNodeAttributesReply 52

typedef struct {
    CARD8 reqType;
    CARD8 sceneReqType;
    CARD16 length;
    CARD32 window;
    CARD32 mask;
} xXpSceneClearNodeAttributesReq;
#define sz_xXpSceneClearNodeAttributesReq 12

/*
 * 0.2. cornerRadius in pixels; shadow: offset, blur radius in pixels and
 * a non-premultiplied ARGB colour (0x80000000 = half-transparent black).
 */
typedef struct {
    CARD8 reqType;
    CARD8 sceneReqType;
    CARD16 length;
    CARD32 window;
    CARD32 mask;                /* XpSceneNodeCornerRadius | XpSceneNodeShadow */
    CARD16 cornerRadius;
    CARD16 shadowBlur;
    INT16 shadowDx;
    INT16 shadowDy;
    CARD32 shadowColor;
} xXpSceneSetNodeDecorationReq;
#define sz_xXpSceneSetNodeDecorationReq 24

typedef struct {
    CARD8 reqType;
    CARD8 sceneReqType;
    CARD16 length;
    CARD32 window;
} xXpSceneGetNodeDecorationReq;
#define sz_xXpSceneGetNodeDecorationReq 8

typedef struct {
    BYTE type;                  /* X_Reply */
    BYTE pad1;
    CARD16 sequenceNumber;
    CARD32 length;              /* 0 */
    CARD32 mask;                /* decorations that are set */
    CARD16 cornerRadius;
    CARD16 shadowBlur;
    INT16 shadowDx;
    INT16 shadowDy;
    CARD32 shadowColor;
    CARD32 pad2, pad3;
} xXpSceneGetNodeDecorationReply;
#define sz_xXpSceneGetNodeDecorationReply 32

#endif /* XPRISM_SCENE_PROTO_H */
