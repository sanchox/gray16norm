// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * GstGray16Window — GRAY16_LE → GRAY8 bit-window extractor
 *
 * Algorithm (per frame):
 *   and_mask = bitwise AND of all 16-bit pixel values
 *   or_mask  = bitwise OR  of all 16-bit pixel values
 *   change_mask = and_mask ^ or_mask
 * Then find the first most significant changing bit (MSB): msb = index_of_msb(change_mask).
 * Start bit for 8-bit window is start = max(msb - 7, 0). If change_mask == 0 (all
 * pixels are equal), use start = 0.
 * Output GRAY8: (v >> start) & 0xFF.
 */

#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/video/gstvideofilter.h>
#include <gst/base/gstbasetransform.h>
#include <string.h>

#ifndef PACKAGE
#define PACKAGE "gray16norm"
#endif

GST_DEBUG_CATEGORY_STATIC (gst_gray16window_debug);
#define GST_CAT_DEFAULT gst_gray16window_debug

typedef struct _GstGray16Window {
  GstVideoFilter parent;
  GstVideoInfo in_info;
  GstVideoInfo out_info;
} GstGray16Window;

typedef struct _GstGray16WindowClass {
  GstVideoFilterClass parent_class;
} GstGray16WindowClass;

#define GST_TYPE_GRAY16WINDOW            (gst_gray16window_get_type())
#define GST_GRAY16WINDOW(obj)            (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_GRAY16WINDOW,GstGray16Window))
#define GST_GRAY16WINDOW_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_GRAY16WINDOW,GstGray16WindowClass))
#define GST_IS_GRAY16WINDOW(obj)         (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_GRAY16WINDOW))
#define GST_IS_GRAY16WINDOW_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_GRAY16WINDOW))

G_DEFINE_TYPE (GstGray16Window, gst_gray16window, GST_TYPE_VIDEO_FILTER);

/* Pad templates */
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
    GST_STATIC_CAPS ("video/x-raw, format=(string)GRAY8")
);

static gboolean
gst_gray16window_set_info (GstVideoFilter * vfilter,
                           GstCaps * incaps, GstVideoInfo * in_info,
                           GstCaps * outcaps, GstVideoInfo * out_info)
{
  GstGray16Window *self = GST_GRAY16WINDOW (vfilter);

  self->in_info = *in_info;
  self->out_info = *out_info;

  if (GST_VIDEO_INFO_FORMAT (in_info) != GST_VIDEO_FORMAT_GRAY16_LE) {
    GST_ERROR_OBJECT (self, "Only GRAY16_LE supported on sink");
    return FALSE;
  }
  if (GST_VIDEO_INFO_FORMAT (out_info) != GST_VIDEO_FORMAT_GRAY8) {
    GST_ERROR_OBJECT (self, "Only GRAY8 supported on src");
    return FALSE;
  }
  return TRUE;
}

/* helper: count trailing zeros for a 16-bit value; returns 0..16 */
static inline guint
ctz16 (guint16 x)
{
  if (x == 0) return 16;
#if defined(__GNUC__)
  return (guint) __builtin_ctz ((unsigned) x);
#else
  guint n = 0;
  while (((x >> n) & 1u) == 0u && n < 16) n++;
  return n;
#endif
}

/* helper: index of most significant set bit for a 16-bit value; returns 0..15, undefined for x==0 */
static inline guint
msb_index16 (guint16 x)
{
  /* x must be non-zero */
#if defined(__GNUC__)
  /* __builtin_clz is for unsigned int (at least 32 bits). Avoid calling with 0. */
  return (guint) (31u - (guint) __builtin_clz ((unsigned) x));
#else
  guint idx = 0;
  while (x >>= 1) idx++;
  return idx;
#endif
}

static GstFlowReturn
gst_gray16window_transform_frame (GstVideoFilter * vfilter,
                                  GstVideoFrame * inframe,
                                  GstVideoFrame * outframe)
{
  GstGray16Window *self = GST_GRAY16WINDOW (vfilter);
  const gsize width  = GST_VIDEO_FRAME_WIDTH (inframe);
  const gsize height = GST_VIDEO_FRAME_HEIGHT (inframe);
  const guint8 *in_base = GST_VIDEO_FRAME_PLANE_DATA (inframe, 0);
  const gsize in_stride = (gsize) GST_VIDEO_FRAME_PLANE_STRIDE (inframe, 0);
  guint8 *out_base = GST_VIDEO_FRAME_PLANE_DATA (outframe, 0);
  const gsize out_stride = (gsize) GST_VIDEO_FRAME_PLANE_STRIDE (outframe, 0);

  if (G_UNLIKELY (width == 0 || height == 0 || width > G_MAXSIZE / height)) {
    GST_ERROR_OBJECT (self, "invalid frame size: %" G_GSIZE_FORMAT "x%" G_GSIZE_FORMAT,
                      width, height);
    return GST_FLOW_ERROR;
  }

  /* First pass: accumulate AND/OR across the whole frame */
  guint16 and_mask = 0xFFFFu;
  guint16 or_mask  = 0x0000u;
  for (gsize y = 0; y < height; y++) {
    const guint8 *line = in_base + y * in_stride;
    for (gsize x = 0; x < width; x++) {
      guint16 v = GST_READ_UINT16_LE (line + (x << 1));
      and_mask &= v;
      or_mask  |= v;
    }
  }

  const guint16 change_mask = (guint16) (and_mask ^ or_mask);
  guint start_bit;
  if (change_mask == 0) {
    start_bit = 0;
  } else {
    /* Find first changing most significant bit and align an 8-bit window so that
     * this bit becomes the top bit of the window. */
    const guint msb = msb_index16 (change_mask);
    start_bit = (msb >= 7u) ? (msb - 7u) : 0u;
    if (start_bit > 15) start_bit = 0; /* safety */
  }

  GST_LOG_OBJECT (self, "and=0x%04x or=0x%04x change=0x%04x msb-start=%u",
                  and_mask, or_mask, change_mask, start_bit);

  /* Second pass: extract an 8-bit window starting at start_bit */
  for (gsize y = 0; y < height; y++) {
    const guint8 *in_line = in_base + y * in_stride;
    guint8 *out_line = out_base + y * out_stride;
    for (gsize x = 0; x < width; x++) {
      guint16 v = GST_READ_UINT16_LE (in_line + (x << 1));
      guint8 outv = (guint8) ((v >> start_bit) & 0xFFu);
      out_line[x] = outv;
    }
  }
  return GST_FLOW_OK;
}

static GstCaps *
gst_gray16window_transform_caps (GstBaseTransform * trans,
                                 GstPadDirection direction,
                                 GstCaps * caps,
                                 GstCaps * filter)
{
  if (gst_caps_is_any (caps)) {
    GstCaps *tmpl;
    if (direction == GST_PAD_SINK) {
      tmpl = gst_static_pad_template_get_caps (&src_tmpl);
    } else {
      tmpl = gst_static_pad_template_get_caps (&sink_tmpl);
    }
    if (filter) {
      GstCaps *intersection = gst_caps_intersect_full (tmpl, filter,
          GST_CAPS_INTERSECT_FIRST);
      gst_caps_unref (tmpl);
      return intersection;
    }
    return tmpl;
  }

  if (gst_caps_is_empty (caps))
    return gst_caps_new_empty ();

  GstCaps *result = gst_caps_new_empty ();
  const guint n = gst_caps_get_size (caps);
  for (guint i = 0; i < n; i++) {
    const GstStructure *s = gst_caps_get_structure (caps, i);
    if (direction == GST_PAD_SINK) {
      GstStructure *s_out = gst_structure_copy (s);
      gst_structure_set (s_out, "format", G_TYPE_STRING, "GRAY8", NULL);
      gst_caps_append_structure (result, s_out);
    } else {
      GstStructure *s_in = gst_structure_copy (s);
      gst_structure_set (s_in, "format", G_TYPE_STRING, "GRAY16_LE", NULL);
      gst_caps_append_structure (result, s_in);
    }
  }
  if (filter) {
    GstCaps *intersection = gst_caps_intersect_full (result, filter,
        GST_CAPS_INTERSECT_FIRST);
    gst_caps_unref (result);
    return intersection;
  }
  return result;
}

static void
gst_gray16window_class_init (GstGray16WindowClass * klass)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstVideoFilterClass *vf_class = GST_VIDEO_FILTER_CLASS (klass);
  GstBaseTransformClass *bt_class = GST_BASE_TRANSFORM_CLASS (klass);

  gst_element_class_set_static_metadata (element_class,
      "Gray16 window extractor", "Filter/Effect/Video",
      "Extracts 8-bit window from 16-bit grayscale based on first changing bit",
      "GstGray16Norm Authors");

  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&sink_tmpl));
  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&src_tmpl));

  vf_class->set_info = gst_gray16window_set_info;
  vf_class->transform_frame = gst_gray16window_transform_frame;
  bt_class->transform_caps = gst_gray16window_transform_caps;

  GST_DEBUG_CATEGORY_INIT (gst_gray16window_debug, "gray16window", 0,
      "GRAY16 bit-window extractor to GRAY8");
}

static void
gst_gray16window_init (GstGray16Window * self)
{
  (void) self;
}

/* Type is exported for registration from gstgray16plugin.c */
