// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * gray16color — fast table-driven Gray16 → Color converter without normalization.
 *
 * - Sink caps:  video/x-raw, format=GRAY16_LE
 * - Src caps:   video/x-raw, format={ RGBx, BGRx, RGBA, BGRA, RGB16, BGR16, RGB15 }
 *
 * The element performs a direct lookup: dst_pixel = LUT[src16], where LUT has 65536
 * entries prepacked for the negotiated output format. Palettes are taken from
 * generated headers (turbo/viridis/magma/jet/prism). No auto-range or scaling here.
 *
 * Intended for aarch64. NEON is not strictly required since the hot path
 * is a memory-bound lookup + store. Writes are 32-bit aligned when using RGBx/BGRx/RGBA/BGRA.
 */

#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/video/gstvideofilter.h>
#include <gst/base/gstbasetransform.h>
#include <string.h>
#include <stdint.h>

#ifndef PACKAGE
#define PACKAGE "gray16norm"
#endif

/* Palettes: generated at build time via lut_gen.py */
#include "gray16_to_rgb_lut.h"           /* gray16_to_rgb (turbo) */
#include "gray16_to_rgb_lut_viridis.h"   /* gray16_to_rgb_viridis */
#include "gray16_to_rgb_lut_magma.h"     /* gray16_to_rgb_magma */
#include "gray16_to_rgb_lut_jet.h"       /* gray16_to_rgb_jet */
#include "gray16_to_rgb_lut_prism.h"     /* gray16_to_rgb_prism */

GST_DEBUG_CATEGORY_STATIC (gst_gray16color_debug);
#define GST_CAT_DEFAULT gst_gray16color_debug

typedef enum {
  GST_GRAY16COLOR_PALETTE_TURBO = 0,
  GST_GRAY16COLOR_PALETTE_VIRIDIS,
  GST_GRAY16COLOR_PALETTE_MAGMA,
  GST_GRAY16COLOR_PALETTE_JET,
  GST_GRAY16COLOR_PALETTE_PRISM,
} GstGray16ColorPalette;

typedef struct _GstGray16Color {
  GstVideoFilter parent;

  GstVideoInfo in_info;
  GstVideoInfo out_info;

  GstGray16ColorPalette palette;
  guint8 alpha; /* for RGBA/BGRA */

  /* Prepacked LUT for current palette+format; always 65536 entries. */
  GBytes *lut_bytes; /* owns memory */
  const void *lut_ptr; /* mapped pointer */
  gsize lut_stride; /* element size: 2 or 4 bytes */
} GstGray16Color;

typedef struct _GstGray16ColorClass {
  GstVideoFilterClass parent_class;
} GstGray16ColorClass;

#define GST_TYPE_GRAY16COLOR            (gst_gray16color_get_type())
#define GST_GRAY16COLOR(obj)            (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_GRAY16COLOR,GstGray16Color))
#define GST_GRAY16COLOR_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_GRAY16COLOR,GstGray16ColorClass))
#define GST_IS_GRAY16COLOR(obj)         (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_GRAY16COLOR))
#define GST_IS_GRAY16COLOR_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_GRAY16COLOR))

G_DEFINE_TYPE (GstGray16Color, gst_gray16color, GST_TYPE_VIDEO_FILTER);

/* --- pad templates --- */

static GstStaticPadTemplate sink_tmpl =
GST_STATIC_PAD_TEMPLATE (
    "sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-raw, format=(string)GRAY16_LE")
);

static GstStaticPadTemplate src_tmpl =
GST_STATIC_PAD_TEMPLATE (
    "src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-raw, format=(string){ RGBx, BGRx, RGBA, BGRA, RGB16, BGR16, RGB15 }")
);

/* --- properties --- */
enum {
  PROP0,
  PROP_PALETTE,
  PROP_ALPHA,
};

static inline const uint8_t (*get_palette_triplets (GstGray16Color *self))[3]
{
  switch (self->palette) {
    default:
    case GST_GRAY16COLOR_PALETTE_TURBO:   return gray16_to_rgb;
    case GST_GRAY16COLOR_PALETTE_VIRIDIS: return gray16_to_rgb_viridis;
    case GST_GRAY16COLOR_PALETTE_MAGMA:   return gray16_to_rgb_magma;
    case GST_GRAY16COLOR_PALETTE_JET:     return gray16_to_rgb_jet;
    case GST_GRAY16COLOR_PALETTE_PRISM:   return gray16_to_rgb_prism;
  }
}

static inline void free_lut (GstGray16Color *self)
{
  if (self->lut_bytes) {
    g_bytes_unref (self->lut_bytes);
    self->lut_bytes = NULL;
    self->lut_ptr = NULL;
    self->lut_stride = 0;
  }
}

static gboolean build_lut_for_format (GstGray16Color *self)
{
  const GstVideoFormat fmt = GST_VIDEO_INFO_FORMAT (&self->out_info);
  const uint8_t (*pal)[3] = get_palette_triplets (self);
  gsize elem_size = 0;

  if (fmt == GST_VIDEO_FORMAT_RGBx || fmt == GST_VIDEO_FORMAT_BGRx ||
      fmt == GST_VIDEO_FORMAT_RGBA || fmt == GST_VIDEO_FORMAT_BGRA) {
    elem_size = 4;
  } else if (fmt == GST_VIDEO_FORMAT_RGB16 || fmt == GST_VIDEO_FORMAT_BGR16 ||
             fmt == GST_VIDEO_FORMAT_RGB15) {
    elem_size = 2;
  } else {
    GST_ERROR_OBJECT (self, "Unsupported output format in gray16color");
    return FALSE;
  }

  gsize bytes = 65536u * elem_size;
  guint8 *mem = g_malloc (bytes);
  if (!mem) return FALSE;

  if (elem_size == 4) {
    guint32 *L = (guint32 *)mem;
    const gboolean is_rgbx = (fmt == GST_VIDEO_FORMAT_RGBx);
    const gboolean is_bgrx = (fmt == GST_VIDEO_FORMAT_BGRx);
    const gboolean is_rgba = (fmt == GST_VIDEO_FORMAT_RGBA);
    const gboolean is_bgra = (fmt == GST_VIDEO_FORMAT_BGRA);
    const guint32 A = ((guint32)self->alpha) << 24;
    for (guint i = 0; i < 65536; i++) {
      const guint8 r = pal[i][0], g = pal[i][1], b = pal[i][2];
      guint32 px;
      if (is_rgbx)      px = ((guint32)r << 16) | ((guint32)g << 8) | (guint32)b;
      else if (is_bgrx) px = ((guint32)b << 16) | ((guint32)g << 8) | (guint32)r;
      else if (is_rgba) px = A | ((guint32)r << 16) | ((guint32)g << 8) | (guint32)b;
      else /* BGRA */   px = A | ((guint32)b << 16) | ((guint32)g << 8) | (guint32)r;
      L[i] = px;
    }
  } else {
    guint16 *L = (guint16 *)mem;
    const gboolean is_rgb16 = (fmt == GST_VIDEO_FORMAT_RGB16);
    const gboolean is_bgr16 = (fmt == GST_VIDEO_FORMAT_BGR16);
    const gboolean is_rgb15 = (fmt == GST_VIDEO_FORMAT_RGB15);
    for (guint i = 0; i < 65536; i++) {
      const guint8 r8 = pal[i][0], g8 = pal[i][1], b8 = pal[i][2];
      guint16 px;
      if (is_rgb16) {
        /* 5:6:5 little-endian packing */
        guint16 r = (guint16)(r8 >> 3);
        guint16 g = (guint16)(g8 >> 2);
        guint16 b = (guint16)(b8 >> 3);
        px = (r << 11) | (g << 5) | b;
      } else if (is_bgr16) {
        guint16 r = (guint16)(r8 >> 3);
        guint16 g = (guint16)(g8 >> 2);
        guint16 b = (guint16)(b8 >> 3);
        px = (b << 11) | (g << 5) | r;
      } else /* RGB15: xRGB1555 (top bit zero) */ {
        guint16 r = (guint16)(r8 >> 3);
        guint16 g = (guint16)(g8 >> 3);
        guint16 b = (guint16)(b8 >> 3);
        px = (r << 10) | (g << 5) | b;
      }
      L[i] = px;
    }
  }

  free_lut (self);
  self->lut_bytes = g_bytes_new_take (mem, bytes);
  self->lut_ptr = g_bytes_get_data (self->lut_bytes, NULL);
  self->lut_stride = elem_size;
  return TRUE;
}

static gboolean
gst_gray16color_set_info (GstVideoFilter *vf, GstCaps *incaps, GstVideoInfo *in_info,
                          GstCaps *outcaps, GstVideoInfo *out_info)
{
  GstGray16Color *self = GST_GRAY16COLOR (vf);
  self->in_info = *in_info;
  self->out_info = *out_info;

  if (GST_VIDEO_INFO_FORMAT (in_info) != GST_VIDEO_FORMAT_GRAY16_LE) {
    GST_ERROR_OBJECT (self, "Only GRAY16_LE supported on sink");
    return FALSE;
  }

  switch (GST_VIDEO_INFO_FORMAT (out_info)) {
    case GST_VIDEO_FORMAT_RGBx:
    case GST_VIDEO_FORMAT_BGRx:
    case GST_VIDEO_FORMAT_RGBA:
    case GST_VIDEO_FORMAT_BGRA:
    case GST_VIDEO_FORMAT_RGB16:
    case GST_VIDEO_FORMAT_BGR16:
    case GST_VIDEO_FORMAT_RGB15:
      break;
    default:
      GST_ERROR_OBJECT (self, "Unsupported output format");
      return FALSE;
  }

  return build_lut_for_format (self);
}

static GstFlowReturn
gst_gray16color_transform_frame (GstVideoFilter *vf, GstVideoFrame *inframe, GstVideoFrame *outframe)
{
  GstGray16Color *self = GST_GRAY16COLOR (vf);
  const guint8 *in_base = GST_VIDEO_FRAME_PLANE_DATA (inframe, 0);
  guint8 *out_base = GST_VIDEO_FRAME_PLANE_DATA (outframe, 0);
  const gsize in_stride = (gsize) GST_VIDEO_FRAME_PLANE_STRIDE (inframe, 0);
  const gsize out_stride = (gsize) GST_VIDEO_FRAME_PLANE_STRIDE (outframe, 0);
  const gsize w = GST_VIDEO_FRAME_WIDTH (inframe);
  const gsize h = GST_VIDEO_FRAME_HEIGHT (inframe);

  const guint format = GST_VIDEO_INFO_FORMAT (&self->out_info);
  const gsize elem = self->lut_stride; /* 2 or 4 */
  const guint8 *lut = (const guint8 *) self->lut_ptr;

  for (gsize y = 0; y < h; y++) {
    const guint16 *src = (const guint16 *)(in_base + y * in_stride);
    guint8 *dst = out_base + y * out_stride;
    for (gsize x = 0; x < w; x++) {
      const guint16 v = src[x]; /* GRAY16_LE, platform is LE */
      const guint8 *p = lut + ((gsize)v * elem);
      if (elem == 4) {
        /* 32-bit store */
        ((guint32*)dst)[x] = *(const guint32*)p;
      } else {
        ((guint16*)dst)[x] = *(const guint16*)p;
      }
    }
  }

  return GST_FLOW_OK;
}

static GstCaps *
gst_gray16color_transform_caps (GstBaseTransform *trans, GstPadDirection direction,
                                GstCaps *caps, GstCaps *filter)
{
  if (gst_caps_is_any (caps)) {
    GstCaps *tmpl = (direction == GST_PAD_SINK)
      ? gst_static_pad_template_get_caps (&src_tmpl)
      : gst_static_pad_template_get_caps (&sink_tmpl);
    if (filter) {
      GstCaps *intersection = gst_caps_intersect_full (tmpl, filter, GST_CAPS_INTERSECT_FIRST);
      gst_caps_unref (tmpl);
      return intersection;
    }
    return tmpl;
  }
  if (gst_caps_is_empty (caps))
    return gst_caps_new_empty ();

  GstCaps *result = gst_caps_new_empty();
  const guint n = gst_caps_get_size (caps);
  for (guint i = 0; i < n; i++) {
    const GstStructure *s = gst_caps_get_structure (caps, i);
    if (direction == GST_PAD_SINK) {
      /* From sink (GRAY16_LE) to src: advertise all supported colors */
      static const char *fmts[] = {"RGBx","BGRx","RGBA","BGRA","RGB16","BGR16","RGB15"};
      for (guint k=0;k<G_N_ELEMENTS(fmts);k++) {
        GstStructure *sc = gst_structure_copy (s);
        gst_structure_set (sc, "format", G_TYPE_STRING, fmts[k], NULL);
        /* Drop YUV-specific fields that should not appear on RGB outputs. */
        gst_structure_remove_field (sc, "colorimetry");
        gst_structure_remove_field (sc, "chroma-site");
        gst_structure_remove_field (sc, "range");
        gst_caps_append_structure (result, sc);
      }
    } else {
      GstStructure *s2 = gst_structure_copy (s);
      gst_structure_set (s2, "format", G_TYPE_STRING, "GRAY16_LE", NULL);
      /* GRAY16 caps should not carry YUV colorimetry/chroma metadata. */
      gst_structure_remove_field (s2, "colorimetry");
      gst_structure_remove_field (s2, "chroma-site");
      gst_structure_remove_field (s2, "range");
      gst_caps_append_structure (result, s2);
    }
  }

  if (filter) {
    GstCaps *intersection = gst_caps_intersect_full (result, filter, GST_CAPS_INTERSECT_FIRST);
    gst_caps_unref (result);
    return intersection;
  }
  return result;
}

static void gst_gray16color_set_property (GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
  GstGray16Color *self = GST_GRAY16COLOR (object);
  switch (prop_id) {
    case PROP_PALETTE: {
      const gchar *s = g_value_get_string (value);
      GstGray16ColorPalette p = GST_GRAY16COLOR_PALETTE_TURBO;
      if (s) {
        if (g_ascii_strcasecmp (s, "turbo") == 0) p = GST_GRAY16COLOR_PALETTE_TURBO;
        else if (g_ascii_strcasecmp (s, "viridis") == 0) p = GST_GRAY16COLOR_PALETTE_VIRIDIS;
        else if (g_ascii_strcasecmp (s, "magma") == 0) p = GST_GRAY16COLOR_PALETTE_MAGMA;
        else if (g_ascii_strcasecmp (s, "jet") == 0) p = GST_GRAY16COLOR_PALETTE_JET;
        else if (g_ascii_strcasecmp (s, "prism") == 0) p = GST_GRAY16COLOR_PALETTE_PRISM;
      }
      if (p != self->palette) {
        self->palette = p;
        /* During construction, out_info.finfo may be NULL. Avoid
         * GST_VIDEO_INFO_FORMAT() until set_info() initializes it. */
        if (self->out_info.finfo != NULL)
          (void) build_lut_for_format (self);
      }
      break; }
    case PROP_ALPHA: {
      guint a = g_value_get_uint (value);
      if (a > 255) a = 255;
      if (self->alpha != (guint8)a) {
        self->alpha = (guint8)a;
        /* Avoid dereferencing out_info.finfo before it is set. */
        if (self->out_info.finfo != NULL)
          (void) build_lut_for_format (self);
      }
      break; }
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
  }
}

static void gst_gray16color_get_property (GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
  GstGray16Color *self = GST_GRAY16COLOR (object);
  switch (prop_id) {
    case PROP_PALETTE: {
      const char *name = "turbo";
      switch (self->palette) {
        case GST_GRAY16COLOR_PALETTE_TURBO: name = "turbo"; break;
        case GST_GRAY16COLOR_PALETTE_VIRIDIS: name = "viridis"; break;
        case GST_GRAY16COLOR_PALETTE_MAGMA: name = "magma"; break;
        case GST_GRAY16COLOR_PALETTE_JET: name = "jet"; break;
        case GST_GRAY16COLOR_PALETTE_PRISM: name = "prism"; break;
      }
      g_value_set_string (value, name);
      break; }
    case PROP_ALPHA:
      g_value_set_uint (value, self->alpha);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
  }
}

static void gst_gray16color_finalize (GObject *object)
{
  GstGray16Color *self = GST_GRAY16COLOR (object);
  free_lut (self);
  G_OBJECT_CLASS (gst_gray16color_parent_class)->finalize (object);
}

static void gst_gray16color_class_init (GstGray16ColorClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstBaseTransformClass *bt_class = GST_BASE_TRANSFORM_CLASS (klass);
  GstVideoFilterClass *vf_class = GST_VIDEO_FILTER_CLASS (klass);

  gobject_class->finalize = gst_gray16color_finalize;
  gobject_class->set_property = gst_gray16color_set_property;
  gobject_class->get_property = gst_gray16color_get_property;

  gst_element_class_set_static_metadata (element_class,
    "Gray16 to Color (table-driven)", "Filter/Converter/Video",
    "Maps GRAY16_LE to color formats via 16-bit LUT without normalization",
    "GstGray16Norm Project");

  gst_element_class_add_static_pad_template (element_class, &sink_tmpl);
  gst_element_class_add_static_pad_template (element_class, &src_tmpl);

  bt_class->transform_caps = gst_gray16color_transform_caps;
  vf_class->set_info = gst_gray16color_set_info;
  vf_class->transform_frame = gst_gray16color_transform_frame;

  g_object_class_install_property (gobject_class, PROP_PALETTE,
    g_param_spec_string ("palette", "Palette",
      "Color palette: turbo, viridis, magma, jet, prism",
      "turbo",
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_ALPHA,
    g_param_spec_uint ("alpha", "Alpha",
      "Alpha value for RGBA/BGRA (0..255)",
      0, 255, 255,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  GST_DEBUG_CATEGORY_INIT (gst_gray16color_debug, "gray16color", 0, "Gray16→Color LUT");
}

static void gst_gray16color_init (GstGray16Color *self)
{
  memset (&self->in_info, 0, sizeof (self->in_info));
  memset (&self->out_info, 0, sizeof (self->out_info));
  self->palette = GST_GRAY16COLOR_PALETTE_TURBO;
  self->alpha = 255;
  self->lut_bytes = NULL;
  self->lut_ptr = NULL;
  self->lut_stride = 0;
}
