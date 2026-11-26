/*
 * SPDX-License-Identifier: MIT
 *
 * Colour conversion of window contents for the built-in compositor
 * (XPRISM-COLOR, doc/color.md) on the GPU. The server does the same on
 * the CPU (Xext/composite/compcolorext.c) where this declines; the
 * steps and constants are those of include/compositeext.h and
 * Xext/composite/compcolor.c.
 */
#include <dix-config.h>

#include "glamor_priv.h"
#include "glamor_transform.h"

#include "compositeext.h"

struct color_args {
    glamor_pixmap_fbo *src;
    int src_w, src_h;
    const CompositeColorConvertRec *conv;
};

static Bool
use_color(DrawablePtr drawable, GCPtr gc, glamor_program *prog, void *arg)
{
    struct color_args *args = arg;
    const CompositeColorConvertRec *c = args->conv;
    GLfloat m[9];

    glamor_bind_texture(glamor_get_screen_private(drawable->pScreen),
                        GL_TEXTURE0, args->src, FALSE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    /* column-major: GLES2 cannot transpose on upload */
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            m[j * 3 + i] = c->matrix[i][j];

    glUniform2f(prog->fill_offset_uniform, 0, 0);
    glUniform2f(prog->fill_size_inv_uniform, 1.0f / args->src_w, 1.0f / args->src_h);
    glUniformMatrix3fv(glGetUniformLocation(prog->prog, "color_mat"), 1, GL_FALSE, m);
    glUniform4f(glGetUniformLocation(prog->prog, "color_tf"),
                c->transfer, c->refWhite, c->maxLum, c->hlgGamma);
    glUniform3f(glGetUniformLocation(prog->prog, "color_luma"),
                c->luma[0], c->luma[1], c->luma[2]);
    glUniform3f(glGetUniformLocation(prog->prog, "color_out_luma"),
                c->outLuma[0], c->outLuma[1], c->outLuma[2]);
    glUniform4f(glGetUniformLocation(prog->prog, "color_tone"),
                c->tone ? 1.0f : 0.0f, c->toneKnee, c->toneSrcPQ, c->toneMaxL);
    glUniform1f(glGetUniformLocation(prog->prog, "color_ks"), c->toneKS);
    glUniform3f(glGetUniformLocation(prog->prog, "color_out"),
                c->outTransfer, c->outRefWhite, c->outPeak);
    return TRUE;
}

#define COLOR_FS_VARS                                                   \
    "#ifdef GL_ES\n"                                                    \
    "precision highp float;\n"                                          \
    "#endif\n"                                                          \
    "uniform mat3 color_mat;\n"                                         \
    "uniform vec4 color_tf;\n"  /* transfer, refWhite, maxLum, HLG gamma */ \
    "uniform vec3 color_luma;\n"                                        \
    "uniform vec3 color_out_luma;\n"                                    \
    "uniform vec4 color_tone;\n" /* on, knee, PQ(maxLum), maxL */      \
    "uniform float color_ks;\n"                                         \
    "uniform vec3 color_out;\n" /* transfer, refWhite, peak */         \
    "float pq_to_nits(float v) {\n"                                     \
    "  float p = pow(clamp(v, 0.0, 1.0), 1.0 / 78.84375);\n"            \
    "  float num = max(p - 0.8359375, 0.0);\n"                          \
    "  return 10000.0 * pow(num / (18.8515625 - 18.6875 * p), 1.0 / 0.1593017578125);\n" \
    "}\n"                                                               \
    "float nits_to_pq(float n) {\n"                                     \
    "  float p = pow(clamp(n / 10000.0, 0.0, 1.0), 0.1593017578125);\n" \
    "  return pow((0.8359375 + 18.8515625 * p) / (1.0 + 18.6875 * p), 78.84375);\n" \
    "}\n"                                                               \
    "float srgb_dec(float v) {\n"                                       \
    "  return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);\n" \
    "}\n"                                                               \
    "float srgb_enc(float v) {\n"                                       \
    "  return v <= 0.0031308 ? v * 12.92 : 1.055 * pow(v, 1.0 / 2.4) - 0.055;\n" \
    "}\n"                                                               \
    "float hlg_scene(float v) {\n"                                      \
    "  return v <= 0.5 ? v * v / 3.0 :\n"                               \
    "    (exp((v - 0.55991073) / 0.17883277) + 0.28466892) / 12.0;\n"  \
    "}\n"                                                               \
    "vec3 color_decode(vec3 c) {\n"                                     \
    "  float tf = color_tf.x;\n"                                        \
    "  if (tf < 0.5) return vec3(srgb_dec(c.r), srgb_dec(c.g), srgb_dec(c.b));\n" \
    "  if (tf < 1.5) return pow(c, vec3(2.2));\n"                       \
    "  if (tf < 2.5) return c;\n"                                       \
    "  if (tf < 3.5)\n"                                                 \
    "    return vec3(pq_to_nits(c.r), pq_to_nits(c.g), pq_to_nits(c.b)) / color_tf.y;\n" \
    "  vec3 e = vec3(hlg_scene(c.r), hlg_scene(c.g), hlg_scene(c.b));\n" \
    "  float ys = dot(color_luma, e);\n"                                \
    "  float k = ys > 0.0 ? color_tf.z * pow(ys, color_tf.w - 1.0) : 0.0;\n" \
    "  return k * e / color_tf.y;\n"                                    \
    "}\n"                                                               \
    "vec3 color_encode(vec3 o) {\n"                                     \
    "  float tf = color_out.x;\n"                                       \
    "  if (tf < 0.5) return vec3(srgb_enc(o.r), srgb_enc(o.g), srgb_enc(o.b));\n" \
    "  if (tf < 1.5) return pow(o, vec3(1.0 / 2.2));\n"                 \
    "  if (tf < 2.5) return o;\n"                                       \
    "  o *= color_out.y;\n"                                             \
    "  return vec3(nits_to_pq(o.r), nits_to_pq(o.g), nits_to_pq(o.b));\n" \
    "}\n"                                                               \
    "float gamut_scale(float c, float y, float s) {\n"                  \
    "  float p = color_out.z;\n"                                        \
    "  if (c > p && c - y > 0.0) return min(s, (p - y) / (c - y));\n"   \
    "  if (c < 0.0) return min(s, y / (y - c));\n"                      \
    "  return s;\n"                                                     \
    "}\n"

#define COLOR_FS_EXEC                                                   \
    "  vec4 s = texture(sampler, fill_pos);\n"                          \
    "  float a = COLOR_OPAQUE ? 1.0 : s.a;\n"                           \
    "  if (a <= 0.0) {\n"                                               \
    "    frag_color = vec4(0.0);\n"                                     \
    "  } else {\n"                                                      \
    "    vec3 o = color_mat * color_decode(min(s.rgb / a, vec3(1.0)));\n" \
    "    float y = dot(color_out_luma, o);\n"                           \
    "    if (color_tone.x > 0.5 && y > color_tone.y) {\n"               \
    "      float ks = color_ks;\n"                                      \
    "      float e1 = min(nits_to_pq(y * color_tf.y) / color_tone.z, 1.0);\n" \
    "      float t = (e1 - ks) / (1.0 - ks), t2 = t * t, t3 = t2 * t;\n" \
    "      float e2 = (2.0 * t3 - 3.0 * t2 + 1.0) * ks + (t3 - 2.0 * t2 + t) * (1.0 - ks) +\n" \
    "                 (-2.0 * t3 + 3.0 * t2) * color_tone.w;\n"         \
    "      float mapped = pq_to_nits(e2 * color_tone.z) / color_tf.y;\n" \
    "      o *= mapped / y;\n"                                          \
    "      y = mapped;\n"                                               \
    "    }\n"                                                           \
    "    y = clamp(y, 0.0, color_out.z);\n"                             \
    "    float sc = gamut_scale(o.r, y, gamut_scale(o.g, y, gamut_scale(o.b, y, 1.0)));\n" \
    "    o = clamp(vec3(y) + sc * (o - vec3(y)), 0.0, color_out.z);\n"  \
    "    vec3 enc = clamp(color_encode(o), 0.0, 1.0);\n"                \
    "    frag_color = vec4(enc * a, a);\n"                              \
    "  }\n"

static const glamor_facet glamor_facet_color = {
    "color",
    .vs_vars = "in vec2 primitive;\n",
    .vs_exec = (GLAMOR_POS(gl_Position, primitive.xy)
                "       fill_pos = (fill_offset + primitive.xy) * fill_size_inv;\n"),
    .fs_vars = COLOR_FS_VARS,
    .fs_exec = COLOR_FS_EXEC,
    .locations = glamor_program_location_fillsamp | glamor_program_location_fillpos,
    .use = use_color,
};

static Bool
glamor_color_convert(PixmapPtr src, PixmapPtr dst, RegionPtr region,
                     const CompositeColorConvertRec *conv)
{
    ScreenPtr screen = dst->drawable.pScreen;
    glamor_screen_private *glamor_priv = glamor_get_screen_private(screen);
    int depth = src->drawable.depth;
    int nbox = RegionNumRects(region), dst_off_x, dst_off_y;
    BoxPtr box = RegionRects(region);
    glamor_program *prog;
    GLshort *v;
    char *vbo_offset;

    if ((depth != 24 && depth != 30 && depth != 32) ||
        dst->drawable.depth != depth || src == dst || src->drawable.width != dst->drawable.width ||
        src->drawable.height != dst->drawable.height ||
        !glamor_pixmap_has_fbo(src) || !glamor_pixmap_has_fbo(dst) ||
        glamor_pixmap_is_large(src) || glamor_pixmap_is_large(dst))
        return FALSE;
    if (!nbox)
        return TRUE;

    prog = &glamor_priv->color_prog[depth != 32];
    if (prog->failed)
        return FALSE;

    glamor_make_current(glamor_priv);
    if (!prog->prog &&
        !glamor_build_program(screen, prog, &glamor_facet_color, NULL, NULL,
                              depth != 32 ? "#define COLOR_OPAQUE true\n"
                                          : "#define COLOR_OPAQUE false\n"))
        return FALSE;
    if (!glamor_set_alu(&dst->drawable, GXcopy))
        return FALSE;

    struct color_args args = {
        .src = glamor_pixmap_fbo_at(glamor_get_pixmap_private(src), 0),
        .src_w = src->drawable.width,
        .src_h = src->drawable.height,
        .conv = conv,
    };
    if (!glamor_use_program(&dst->drawable, NULL, prog, &args))
        return FALSE;

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
    if (glamor_set_destination_drawable(&dst->drawable, 0, FALSE, FALSE,
                                        prog->matrix_uniform,
                                        &dst_off_x, &dst_off_y))
        glamor_glDrawArrays_GL_QUADS(glamor_priv, nbox);
    glDisableVertexAttribArray(GLAMOR_VERTEX_POS);
    return TRUE;
}

void
glamor_color_init(ScreenPtr screen)
{
    glamor_screen_private *glamor_priv = glamor_get_screen_private(screen);

    if (!(glamor_priv->flags & GLAMOR_NO_RENDER_ACCEL) &&
        !getenv("XPRISM_NO_GPU_COLOR"))
        CompositeSetColorConvert(screen, glamor_color_convert);
}

void
glamor_color_fini(ScreenPtr screen)
{
    CompositeSetColorConvert(screen, NULL);
}
