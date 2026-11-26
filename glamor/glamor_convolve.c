/*
 * SPDX-License-Identifier: MIT
 *
 * Render's convolution filter on the GPU.
 *
 * Without this, any Composite whose source uses the convolution filter
 * falls back to software: the pixmaps are read back from the GPU,
 * convolved on the CPU and uploaded again. The compositor's background
 * blur is made of such passes, so on real hardware it stalled the GPU on
 * every repaint.
 *
 * Each destination pixel is computed as pixman does: the taps start at
 * floor(center - ((w - 1) / 2, (h - 1) / 2)), weighted by the kernel in
 * row-major order, with pixels outside the source either repeating the
 * edge (RepeatPad) or transparent (RepeatNone).
 */
#include <dix-config.h>

#include "glamor_priv.h"
#include "glamor_transform.h"

#include "mipict.h"

/* taps per kernel, as 16 vec4 uniforms so GLES2 limits are met */
#define CONV_MAX_TAPS 64

struct convolve_args {
    glamor_pixmap_fbo *src;
    int dx, dy;
    int src_w, src_h;
    const PicturePtr source;
};

static Bool
use_convolve(DrawablePtr drawable, GCPtr gc, glamor_program *prog, void *arg)
{
    struct convolve_args *args = arg;
    PicturePtr source = args->source;
    int w = xFixedToInt(source->filter_params[0]);
    int h = xFixedToInt(source->filter_params[1]);
    GLfloat kernel[CONV_MAX_TAPS] = { 0 };

    glamor_bind_texture(glamor_get_screen_private(drawable->pScreen),
                        GL_TEXTURE0, args->src, FALSE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    /* glamor_program has no slots for these; looking them up costs far
       less than the pass itself */
    GLint conv_kernel_uniform = glGetUniformLocation(prog->prog, "conv_kernel");
    GLint conv_dims_uniform = glGetUniformLocation(prog->prog, "conv_dims");
    GLint conv_src_uniform = glGetUniformLocation(prog->prog, "conv_src");

    for (int i = 0; i < w * h; i++)
        kernel[i] = xFixedToFloat(source->filter_params[2 + i]);

    glUniform2f(prog->fill_offset_uniform, args->dx, args->dy);
    glUniform2f(prog->fill_size_inv_uniform, 1.0f / args->src_w, 1.0f / args->src_h);
    glUniform4fv(conv_kernel_uniform, CONV_MAX_TAPS / 4, kernel);
    glUniform4f(conv_dims_uniform, w, h, w * h,
                source->repeatType == RepeatPad ? 1.0f : 0.0f);
    /* source size, the first tap's offset from the pixel center */
    glUniform4f(conv_src_uniform, args->src_w, args->src_h,
                (w - 1) / 2.0f, (h - 1) / 2.0f);

    return TRUE;
}

/* One tap: kernel weight k at tap index fi. Pixels outside the source are
   transparent, or the nearest edge pixel with RepeatPad. Sources without
   alpha read as opaque, as pixman reads them. */
#define CONV_FS_VARS                                                    \
    "uniform vec4 conv_kernel[16];\n"                                   \
    "uniform vec4 conv_dims;\n"                                         \
    "uniform vec4 conv_src;\n"                                          \
    "#define CONV_TAP(fi, k) if ((fi) < conv_dims.z) {"                 \
    "  float ty = floor(((fi) + 0.5) / conv_dims.x);"  /* exact even where division is by reciprocal */                           \
    "  vec2 c = first + vec2((fi) - ty * conv_dims.x, ty) + 0.5;"       \
    "  bool inside = all(greaterThanEqual(c, vec2(0.0))) &&"            \
    "                all(lessThan(c, conv_src.xy));"                    \
    "  if (inside || conv_dims.w > 0.5) {"                              \
    "    vec4 s = texture(sampler, clamp(c, vec2(0.5), conv_src.xy - 0.5) / conv_src.xy);" \
    "    if (CONV_OPAQUE) s.a = 1.0;"                                   \
    "    acc += (k) * s;"                                               \
    "  }"                                                               \
    "}\n"

#define CONV_FS_EXEC                                                    \
    "       vec2 p = fill_pos * conv_src.xy;\n"                         \
    "       vec2 first = floor(p - conv_src.zw - 1.0 / 65536.0);\n"     \
    "       vec4 acc = vec4(0.0);\n"                                    \
    "       for (int j = 0; j < 16; j++) {\n"                           \
    "               float f = float(j) * 4.0;\n"                        \
    "               if (f >= conv_dims.z) break;\n"                     \
    "               vec4 k4 = conv_kernel[j];\n"                        \
    "               CONV_TAP(f, k4.x)\n"                                \
    "               CONV_TAP(f + 1.0, k4.y)\n"                          \
    "               CONV_TAP(f + 2.0, k4.z)\n"                          \
    "               CONV_TAP(f + 3.0, k4.w)\n"                          \
    "       }\n"                                                        \
    "       frag_color = clamp(acc, 0.0, 1.0);\n"

static const glamor_facet glamor_facet_convolve = {
    "convolve",
    .vs_vars = "in vec2 primitive;\n",
    .vs_exec = (GLAMOR_POS(gl_Position, primitive.xy)
                "       fill_pos = (fill_offset + primitive.xy) * fill_size_inv;\n"),
    .fs_vars = CONV_FS_VARS,
    .fs_exec = CONV_FS_EXEC,
    .locations = glamor_program_location_fillsamp | glamor_program_location_fillpos,
    .use = use_convolve,
};

static Bool
glamor_convolve_format_ok(PictFormatShort format)
{
    return format == PICT_a8r8g8b8 || format == PICT_x8r8g8b8;
}

/*
 * Composite source onto dest within region (dest coordinates, clipped),
 * where source uses the convolution filter. Returns FALSE, having drawn
 * nothing, for what this path does not handle.
 */
Bool
glamor_composite_convolution(CARD8 op, PicturePtr source, PicturePtr mask,
                             PicturePtr dest, RegionPtr region,
                             INT16 x_source, INT16 y_source,
                             INT16 x_dest, INT16 y_dest)
{
    ScreenPtr screen = dest->pDrawable->pScreen;
    glamor_screen_private *glamor_priv = glamor_get_screen_private(screen);
    DrawablePtr src_draw = source->pDrawable, dst_draw = dest->pDrawable;
    PixmapPtr src_pixmap, dst_pixmap;
    glamor_pixmap_private *src_priv;
    Bool opaque;
    glamor_program *prog;
    int nbox, w, h, dst_off_x, dst_off_y;
    BoxPtr box;
    GLshort *v;
    char *vbo_offset;

    if (op != PictOpSrc || mask || !src_draw || src_draw->type != DRAWABLE_PIXMAP ||
        source->transform || source->alphaMap || dest->alphaMap ||
        (source->repeatType != RepeatPad && source->repeatType != RepeatNone) ||
        !glamor_convolve_format_ok(source->format) ||
        !glamor_convolve_format_ok(dest->format) ||
        source->filter_nparams < 2)
        return FALSE;

    w = xFixedToInt(source->filter_params[0]);
    h = xFixedToInt(source->filter_params[1]);
    if (w <= 0 || h <= 0 || w * h > CONV_MAX_TAPS ||
        source->filter_nparams != w * h + 2)
        return FALSE;

    src_pixmap = (PixmapPtr) src_draw;
    dst_pixmap = glamor_get_drawable_pixmap(dst_draw);
    if (src_pixmap == dst_pixmap ||
        !glamor_pixmap_has_fbo(src_pixmap) || !glamor_pixmap_has_fbo(dst_pixmap) ||
        glamor_pixmap_is_large(src_pixmap) || glamor_pixmap_is_large(dst_pixmap))
        return FALSE;
    src_priv = glamor_get_pixmap_private(src_pixmap);

    opaque = PICT_FORMAT_A(source->format) == 0;
    prog = &glamor_priv->convolve_prog[opaque];
    if (prog->failed)
        return FALSE;

    glamor_make_current(glamor_priv);

    if (!prog->prog) {
        if (!glamor_build_program(screen, prog, &glamor_facet_convolve, NULL, NULL,
                                  opaque ? "#define CONV_OPAQUE true\n"
                                         : "#define CONV_OPAQUE false\n"))
            return FALSE;
    }

    if (!glamor_set_alu(dst_draw, GXcopy))
        return FALSE;

    struct convolve_args args = {
        .src = glamor_pixmap_fbo_at(src_priv, 0),
        /* dest coordinates to source pixmap coordinates */
        .dx = x_source - x_dest - dst_draw->x,
        .dy = y_source - y_dest - dst_draw->y,
        .src_w = src_pixmap->drawable.width,
        .src_h = src_pixmap->drawable.height,
        .source = source,
    };

    if (!glamor_use_program(dst_draw, NULL, prog, &args))
        return FALSE;

    nbox = RegionNumRects(region);
    box = RegionRects(region);
    v = glamor_get_vbo_space(screen, nbox * 8 * sizeof(GLshort), &vbo_offset);
    glEnableVertexAttribArray(GLAMOR_VERTEX_POS);
    glVertexAttribPointer(GLAMOR_VERTEX_POS, 2, GL_SHORT, GL_FALSE,
                          2 * sizeof(GLshort), vbo_offset);
    for (int n = 0; n < nbox; n++, box++) {
        v[0] = box->x1; v[1] = box->y1;
        v[2] = box->x1; v[3] = box->y2;
        v[4] = box->x2; v[5] = box->y2;
        v[6] = box->x2; v[7] = box->y1;
        v += 8;
    }
    glamor_put_vbo_space(screen);

    glDisable(GL_BLEND);

    if (glamor_set_destination_drawable(dst_draw, 0, FALSE, FALSE,
                                        prog->matrix_uniform,
                                        &dst_off_x, &dst_off_y))
        glamor_glDrawArrays_GL_QUADS(glamor_priv, nbox);

    glDisableVertexAttribArray(GLAMOR_VERTEX_POS);
    return TRUE;
}
