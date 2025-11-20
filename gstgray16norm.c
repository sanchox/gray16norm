// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * GstGray16Norm — GRAY16_LE → GRAY8 normalization
 *
 * A simple GStreamer video filter that normalizes 16-bit grayscale frames
 * to 8-bit using either auto (per-frame min/max) or manual range.
 */

#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/video/gstvideofilter.h>
#include <gst/base/gstbasetransform.h>
#include <gst/gstutils.h>
#include <string.h>

#ifndef PACKAGE
#define PACKAGE "gray16norm"
#endif

GST_DEBUG_CATEGORY_STATIC (gst_gray16norm_debug);
#define GST_CAT_DEFAULT gst_gray16norm_debug

typedef struct GstGray16Norm {
	GstVideoFilter parent;

	gboolean auto_range;
	guint16  black_level;
	guint16  white_level;

	GstVideoInfo in_info;
	GstVideoInfo out_info;
} GstGray16Norm;

typedef struct GstGray16NormClass {
	GstVideoFilterClass parent_class;
} GstGray16NormClass;

#define GST_TYPE_GRAY16NORM            (gst_gray16norm_get_type())
#define GST_GRAY16NORM(obj)            (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_GRAY16NORM,GstGray16Norm))
#define GST_GRAY16NORM_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_GRAY16NORM,GstGray16NormClass))
#define GST_IS_GRAY16NORM(obj)         (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_GRAY16NORM))
#define GST_IS_GRAY16NORM_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_GRAY16NORM))

G_DEFINE_TYPE (GstGray16Norm, gst_gray16norm, GST_TYPE_VIDEO_FILTER);

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
		GST_STATIC_CAPS ("video/x-raw, format=(string)GRAY8")
		);

/* No separate helper: transform is optimized to be two-pass, stride-aware, and allocation-free. */

/* --- GstVideoFilter::set_info --- */

static gboolean
gst_gray16norm_set_info (GstVideoFilter * video_filter,
		GstCaps * incaps, GstVideoInfo * in_info,
		GstCaps * outcaps, GstVideoInfo * out_info)
{
	GstGray16Norm *self = GST_GRAY16NORM (video_filter);

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

/* --- GstVideoFilter::transform_frame --- */

static GstFlowReturn
gst_gray16norm_transform_frame (GstVideoFilter * video_filter,
        GstVideoFrame * inframe,
        GstVideoFrame * outframe)
{
    GstGray16Norm *self = GST_GRAY16NORM (video_filter);

    const gsize width  = GST_VIDEO_FRAME_WIDTH (inframe);
    const gsize height = GST_VIDEO_FRAME_HEIGHT (inframe);

    guint8 *out_base   = GST_VIDEO_FRAME_PLANE_DATA (outframe, 0);
    const gsize out_stride = (gsize) GST_VIDEO_FRAME_PLANE_STRIDE (outframe, 0);

    const guint8 *in_base    = GST_VIDEO_FRAME_PLANE_DATA (inframe, 0);
    const gsize in_stride  = (gsize) GST_VIDEO_FRAME_PLANE_STRIDE (inframe, 0);

    if (G_UNLIKELY (width == 0 || height == 0 || width > G_MAXSIZE / height)) {
        GST_ERROR_OBJECT (self, "invalid frame size: %" G_GSIZE_FORMAT "x%" G_GSIZE_FORMAT,
                width, height);
        return GST_FLOW_ERROR;
    }
    /* number of pixels not needed explicitly in the new implementation */

    GST_LOG_OBJECT (self, "frame %" G_GSIZE_FORMAT "x%" G_GSIZE_FORMAT
            " auto=%d black=%u white=%u",
            width, height, self->auto_range,
            (unsigned) self->black_level,
            (unsigned) self->white_level);

    if (G_UNLIKELY (!self->auto_range && self->white_level <= self->black_level)) {
        GST_WARNING_OBJECT (self, "white-level (=%u) <= black-level (=%u); output will be black",
                (unsigned) self->white_level, (unsigned) self->black_level);
    }

    /* Determine min/max */
    guint16 minPixelValue = self->auto_range ? 0xffff : self->black_level;
    guint16 maxPixelValue = self->auto_range ? 0x0000 : self->white_level;

    if (self->auto_range) {
      for (gsize y = 0; y < height; y++) {
        const guint8 *line = in_base + y * in_stride;
        for (gsize x = 0; x < width; x++) {
          const guint16 v = GST_READ_UINT16_LE (line + (x << 1));
          if (v < minPixelValue) minPixelValue = v;
          if (v > maxPixelValue) maxPixelValue = v;
        }
      }
    }

    /* Degenerate range: fill zeros quickly */
    if (G_UNLIKELY (maxPixelValue <= (guint16)(minPixelValue + 1))) {
      for (gsize y = 0; y < height; y++) {
        guint8 *out_line = out_base + y * out_stride;
        memset (out_line, 0, (size_t) width);
      }
      return GST_FLOW_OK;
    }

    /* Fast path for the common full-range manual mapping: v >> 8 */
    if (G_LIKELY (!self->auto_range && minPixelValue == 0 && maxPixelValue == 65535)) {
      for (gsize y = 0; y < height; y++) {
        const guint8 *in_line = in_base + y * in_stride;
        guint8 *out_line = out_base + y * out_stride;
        for (gsize x = 0; x < width; x++) {
          /* For LE 16-bit, the high byte is v >> 8 */
          out_line[x] = in_line[(x << 1) + 1];
        }
      }
      return GST_FLOW_OK;
    }

    /* General path: fixed-point scaling without per-pixel division */
    const guint32 range = (guint32) (maxPixelValue - minPixelValue);
    const guint64 scale_fp = (((guint64)255) << 32) / range; /* Q32.32 */
    const guint64 bias = (1ULL << 31); /* half-up rounding */

    for (gsize y = 0; y < height; y++) {
      const guint8 *in_line = in_base + y * in_stride;
      guint8 *out_line = out_base + y * out_stride;
      for (gsize x = 0; x < width; x++) {
        const guint16 v = GST_READ_UINT16_LE (in_line + (x << 1));
        if (v <= minPixelValue) {
          out_line[x] = 0;
        } else if (v >= maxPixelValue) {
          out_line[x] = 255;
        } else {
          const guint32 t = (guint32) (v - minPixelValue);
          const guint64 prod = ((guint64) t) * scale_fp + bias;
          const guint32 val = (guint32) (prod >> 32);
          out_line[x] = (val > 255u) ? 255u : (guint8) val; /* safety clamp */
        }
      }
    }

    return GST_FLOW_OK;
}

/* --- GstBaseTransform::transform_caps --- */

static GstCaps *
gst_gray16norm_transform_caps (GstBaseTransform * trans,
		GstPadDirection direction,
		GstCaps * caps,
		GstCaps * filter)
{
	/* ANY caps on input → use template caps for the opposite side */
	if (gst_caps_is_any (caps)) {
		GstCaps *tmpl;

		if (direction == GST_PAD_SINK) {
			/* caps are defined for sink, return caps for src */
			tmpl = gst_static_pad_template_get_caps (&src_tmpl);
		} else {
			/* caps are defined for src, return caps for sink */
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

	if (gst_caps_is_empty (caps)) {
		return gst_caps_new_empty ();
	}

	GstCaps *result = gst_caps_new_empty();

	const guint n = gst_caps_get_size (caps);
	for (guint i = 0; i < n; i++) {
		const GstStructure *s = gst_caps_get_structure (caps, i);
		GstStructure *s2 = gst_structure_copy (s);

		if (direction == GST_PAD_SINK) {
			/* input describes caps for sink → return caps for src */
			gst_structure_set (s2,
					"format", G_TYPE_STRING, "GRAY8",
					NULL);
		} else {
			/* input describes caps for src → return caps for sink */
			gst_structure_set (s2,
					"format", G_TYPE_STRING, "GRAY16_LE",
					NULL);
		}

		gst_caps_append_structure (result, s2);
	}

	if (filter) {
		GstCaps *intersection = gst_caps_intersect_full (result, filter,
				GST_CAPS_INTERSECT_FIRST);
		gst_caps_unref (result);
		return intersection;
	}

	return result;
}


/* --- GObject properties --- */

enum {
	PROP_0,
	PROP_AUTO_RANGE,
	PROP_BLACK_LEVEL,
	PROP_WHITE_LEVEL,
};

static void
gst_gray16norm_set_property (GObject * object, const guint prop_id,
		const GValue * value, GParamSpec * pspec)
{
	GstGray16Norm *self = GST_GRAY16NORM (object);

	switch (prop_id) {
		case PROP_AUTO_RANGE:
			self->auto_range = g_value_get_boolean (value);
			break;
		case PROP_BLACK_LEVEL:
			self->black_level = (guint16) g_value_get_uint (value);
			break;
		case PROP_WHITE_LEVEL:
			self->white_level = (guint16) g_value_get_uint (value);
			break;
		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
			break;
	}
}

static void
gst_gray16norm_get_property (GObject * object, const guint prop_id,
		GValue * value, GParamSpec * pspec)
{
	const GstGray16Norm *self = GST_GRAY16NORM (object);

	switch (prop_id) {
		case PROP_AUTO_RANGE:
			g_value_set_boolean (value, self->auto_range);
			break;
		case PROP_BLACK_LEVEL:
			g_value_set_uint (value, self->black_level);
			break;
		case PROP_WHITE_LEVEL:
			g_value_set_uint (value, self->white_level);
			break;
		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
			break;
	}
}

/* --- class / instance init --- */

static void
gst_gray16norm_class_init (GstGray16NormClass * klass)
{
	GObjectClass         *gobject_class = G_OBJECT_CLASS (klass);
	GstElementClass      *element_class = GST_ELEMENT_CLASS (klass);
	GstVideoFilterClass  *video_filter_class = GST_VIDEO_FILTER_CLASS (klass);
	GstBaseTransformClass *bt_class = GST_BASE_TRANSFORM_CLASS (klass);

	gobject_class->set_property = gst_gray16norm_set_property;
	gobject_class->get_property = gst_gray16norm_get_property;

	g_object_class_install_property (
			gobject_class, PROP_AUTO_RANGE,
			g_param_spec_boolean ("auto-range", "Auto range",
				"Compute black/white levels per-frame from min/max",
				TRUE,
				G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

	g_object_class_install_property (
			gobject_class, PROP_BLACK_LEVEL,
			g_param_spec_uint ("black-level", "Black level",
				"Input level mapped to 0 (used when auto-range=false)",
				0, 65535, 0,
				G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

	g_object_class_install_property (
			gobject_class, PROP_WHITE_LEVEL,
			g_param_spec_uint ("white-level", "White level",
				"Input level mapped to 255 (used when auto-range=false)",
				0, 65535, 65535,
				G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

	gst_element_class_set_static_metadata (element_class,
			"Gray16 normalizer", "Filter/Effect/Video",
			"Normalize GRAY16 to GRAY8 with auto or manual range",
			"GstGray16Norm Authors");

	gst_element_class_add_pad_template (
			element_class,
			gst_static_pad_template_get (&sink_tmpl));
	gst_element_class_add_pad_template (
			element_class,
			gst_static_pad_template_get (&src_tmpl));

	video_filter_class->set_info        = gst_gray16norm_set_info;
	video_filter_class->transform_frame = gst_gray16norm_transform_frame;

	bt_class->transform_caps            = gst_gray16norm_transform_caps;

	GST_DEBUG_CATEGORY_INIT (gst_gray16norm_debug, "gray16norm", 0,
			"GRAY16 to GRAY8 normalizer");
}

static void
gst_gray16norm_init (GstGray16Norm * self)
{
	self->auto_range  = TRUE;
	self->black_level = 0;
	self->white_level = 65535;
}

/* --- plugin init --- */

static gboolean
plugin_init (GstPlugin * plugin)
{
	return gst_element_register (plugin,
			"gray16norm",
			GST_RANK_NONE,
			GST_TYPE_GRAY16NORM);
}

GST_PLUGIN_DEFINE (
        GST_VERSION_MAJOR,
        GST_VERSION_MINOR,
        gray16norm,
        "GRAY16 normalization plugin",
        plugin_init,
        "1.0",
        "LGPL-2.1-or-later",
        "GstGray16Norm",
        "https://github.com/sanchox/gray16norm"
        )
