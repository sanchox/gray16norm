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
#include <stdint.h>

/* Mandatory LUTs: generated at build time via Makefile (lut_gen.py --all) */
#include "gray16_to_rgb_lut.h"           /* gray16_to_rgb (turbo) */
#include "gray16_to_rgb_lut_viridis.h"   /* gray16_to_rgb_viridis */
#include "gray16_to_rgb_lut_magma.h"     /* gray16_to_rgb_magma */
#include "gray16_to_rgb_lut_jet.h"       /* gray16_to_rgb_jet */
#include "gray16_to_rgb_lut_prism.h"     /* gray16_to_rgb_prism */

/* Use intrinsics when available (compile-time selection). */
#if defined(__ARM_NEON) || defined(__aarch64__)
#include <arm_neon.h>
#define GST_GRAY16NORM_HAVE_NEON 1
#else
#define GST_GRAY16NORM_HAVE_NEON 0
#endif

#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
/* SSE2 intrinsics for x86/x86_64 */
#include <emmintrin.h>
#define GST_GRAY16NORM_HAVE_SSE2 1
#else
#define GST_GRAY16NORM_HAVE_SSE2 0
#endif

/* GCC/Clang vector extensions (portable vector types) */
#if (defined(__GNUC__) || defined(__clang__)) && !defined(__IBMC__)
#define GST_GRAY16NORM_HAVE_GNU_VECTOR 1
#else
#define GST_GRAY16NORM_HAVE_GNU_VECTOR 0
#endif
#ifndef PACKAGE
#define PACKAGE "gray16norm"
#endif

GST_DEBUG_CATEGORY_STATIC (gst_gray16norm_debug);
#define GST_CAT_DEFAULT gst_gray16norm_debug

typedef enum {
    GST_GRAY16NORM_PALETTE_TURBO = 0,
    GST_GRAY16NORM_PALETTE_VIRIDIS,
    GST_GRAY16NORM_PALETTE_MAGMA,
    GST_GRAY16NORM_PALETTE_JET,
    GST_GRAY16NORM_PALETTE_PRISM,
} GstGray16NormPalette;

typedef struct GstGray16Norm {
	GstVideoFilter parent;

	gboolean auto_range;
	guint16  black_level;
	guint16  white_level;

	/* RGB LUT palette selection (used only when output is RGB) */
	GstGray16NormPalette palette;

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
		GST_STATIC_CAPS ("video/x-raw, format=(string){ GRAY8, RGB }")
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
	if (GST_VIDEO_INFO_FORMAT (out_info) != GST_VIDEO_FORMAT_GRAY8 &&
        GST_VIDEO_INFO_FORMAT (out_info) != GST_VIDEO_FORMAT_RGB) {
        GST_ERROR_OBJECT (self, "Only GRAY8 or RGB supported on src");
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
#if GST_GRAY16NORM_HAVE_NEON && defined(__aarch64__)
      /* Vectorized min/max scan across full frame */
      uint16x8_t minValueVector = vdupq_n_u16 (0xffffu);
      uint16x8_t maxValueVector = vdupq_n_u16 (0x0000u);
      for (gsize y = 0; y < height; y++) {
        const guint8 *line = in_base + y * in_stride;
        const guint16 *line16 = (const guint16 *) line;
        gsize x = 0;
        const gsize w8 = width & ~(gsize)7; /* multiple of 8 */
        for (; x < w8; x += 8) {
          uint16x8_t v = vld1q_u16 (line16 + x);
          minValueVector = vminq_u16 (minValueVector, v);
          maxValueVector = vmaxq_u16 (maxValueVector, v);
        }
        /* tail */
        for (; x < width; x++) {
          const guint16 v = line16[x];
          if (v < minPixelValue) minPixelValue = v;
          if (v > maxPixelValue) maxPixelValue = v;
        }
      }
      /* Reduce vector mins/maxs and merge with scalar tails */
      guint16 vmin_scalar = (guint16) vminvq_u16 (minValueVector);
      guint16 vmax_scalar = (guint16) vmaxvq_u16 (maxValueVector);
      if (vmin_scalar < minPixelValue) minPixelValue = vmin_scalar;
      if (vmax_scalar > maxPixelValue) maxPixelValue = vmax_scalar;
      GST_LOG_OBJECT (self, "auto-range: NEON min=%u max=%u",
                      (unsigned) minPixelValue, (unsigned) maxPixelValue);
#elif GST_GRAY16NORM_HAVE_SSE2
      /* SSE2 path: use bias-xor by 0x8000 to emulate unsigned min/max with signed ops */
      const __m128i vbias = _mm_set1_epi16 ((short)0x8000);
      __m128i vminx = _mm_set1_epi16 ((short)0x7FFF);   /* +32767 */
      __m128i vmaxx = _mm_set1_epi16 ((short)0x8000);   /* -32768 */
      for (gsize y = 0; y < height; y++) {
        const guint8 *line = in_base + y * in_stride;
        const guint16 *line16 = (const guint16 *) line;
        gsize x = 0;
        const gsize w8 = width & ~(gsize)7; /* process 8 pixels */
        for (; x < w8; x += 8) {
          __m128i v = _mm_loadu_si128 ((const __m128i *) (line16 + x));
          __m128i vx = _mm_xor_si128 (v, vbias); /* bias to signed */
          vminx = _mm_min_epi16 (vminx, vx);
          vmaxx = _mm_max_epi16 (vmaxx, vx);
        }
        /* tail */
        for (; x < width; x++) {
          const guint16 v = line16[x];
          if (v < minPixelValue) minPixelValue = v;
          if (v > maxPixelValue) maxPixelValue = v;
        }
      }
      /* Unbias and reduce vectors to scalars */
      __m128i vmin = _mm_xor_si128 (vminx, vbias);
      __m128i vmax = _mm_xor_si128 (vmaxx, vbias);
      guint16 tmpmin[8], tmpmax[8];
      _mm_storeu_si128 ((__m128i *) tmpmin, vmin);
      _mm_storeu_si128 ((__m128i *) tmpmax, vmax);
      for (int i = 0; i < 8; i++) {
        if (tmpmin[i] < minPixelValue) minPixelValue = tmpmin[i];
        if (tmpmax[i] > maxPixelValue) maxPixelValue = tmpmax[i];
      }
      GST_LOG_OBJECT (self, "auto-range: SSE2 min=%u max=%u",
                      (unsigned) minPixelValue, (unsigned) maxPixelValue);
#elif GST_GRAY16NORM_HAVE_GNU_VECTOR
      /* GCC/Clang vector extensions path (vector_size(16) of u16).
       * This path is architecture-agnostic and lets the compiler pick
       * appropriate vector instructions where available. */
      typedef unsigned short u16x8 __attribute__((vector_size(16)));
      const u16x8 vmax_init = (u16x8){0,0,0,0,0,0,0,0};
      const u16x8 vmin_init = (u16x8){0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF,0xFFFF};
      u16x8 vminv = vmin_init;
      u16x8 vmaxv = vmax_init;
      for (gsize y = 0; y < height; y++) {
        const guint8 *line = in_base + y * in_stride;
        const guint16 *line16 = (const guint16 *) line;
        gsize x = 0;
        const gsize w8 = width & ~(gsize)7;
        for (; x < w8; x += 8) {
          /* Unaligned-safe load via memcpy to avoid strict-aliasing/alignment UB */
          u16x8 v;
          __builtin_memcpy(&v, line16 + x, sizeof(v));
          u16x8 mask_lt = (u16x8) (v < vminv);
          u16x8 mask_gt = (u16x8) (v > vmaxv);
          /* select: new_min = mask_lt ? v : vminv */
          vminv = (u16x8) ((vminv & ~mask_lt) | (v & mask_lt));
          vmaxv = (u16x8) ((vmaxv & ~mask_gt) | (v & mask_gt));
        }
        /* tail */
        for (; x < width; x++) {
          const guint16 v = line16[x];
          if (v < minPixelValue) minPixelValue = v;
          if (v > maxPixelValue) maxPixelValue = v;
        }
      }
      /* Reduce vector mins/maxs and merge with scalar tails */
      guint16 tmpmin[8];
      guint16 tmpmax[8];
      __builtin_memcpy(tmpmin, &vminv, sizeof(tmpmin));
      __builtin_memcpy(tmpmax, &vmaxv, sizeof(tmpmax));
      for (int i = 0; i < 8; i++) {
        if (tmpmin[i] < minPixelValue) minPixelValue = tmpmin[i];
        if (tmpmax[i] > maxPixelValue) maxPixelValue = tmpmax[i];
      }
      GST_LOG_OBJECT (self, "auto-range: GNU-Vector min=%u max=%u",
                      (unsigned) minPixelValue, (unsigned) maxPixelValue);
#else
      for (gsize y = 0; y < height; y++) {
        const guint8 *line = in_base + y * in_stride;
        for (gsize x = 0; x < width; x++) {
          const guint16 v = GST_READ_UINT16_LE (line + (x << 1));
          if (v < minPixelValue) minPixelValue = v;
          if (v > maxPixelValue) maxPixelValue = v;
        }
      }
#endif
    }

    /* Degenerate range: fill zeros quickly */
    if (G_UNLIKELY (maxPixelValue <= minPixelValue)) {
      /* Handle both output formats */
      if (GST_VIDEO_INFO_FORMAT (&self->out_info) == GST_VIDEO_FORMAT_RGB) {
        for (gsize y = 0; y < height; y++) {
          guint8 *out_line = out_base + y * out_stride;
          memset (out_line, 0, (size_t) (width * 3));
        }
      } else {
        for (gsize y = 0; y < height; y++) {
          guint8 *out_line = out_base + y * out_stride;
          memset (out_line, 0, (size_t) width);
        }
      }
      return GST_FLOW_OK;
    }

    /* If RGB output requested, take a dedicated path using LUT on normalized 16-bit */
    if (GST_VIDEO_INFO_FORMAT (&self->out_info) == GST_VIDEO_FORMAT_RGB) {
      const guint32 range = (guint32) (maxPixelValue - minPixelValue);

      /* Pick LUT pointer based on palette */
      const uint8_t (*lut)[3] = NULL;
      switch (self->palette) {
        case GST_GRAY16NORM_PALETTE_TURBO:   lut = gray16_to_rgb; break;
        case GST_GRAY16NORM_PALETTE_VIRIDIS: lut = gray16_to_rgb_viridis; break;
        case GST_GRAY16NORM_PALETTE_MAGMA:   lut = gray16_to_rgb_magma; break;
        case GST_GRAY16NORM_PALETTE_JET:     lut = gray16_to_rgb_jet; break;
        case GST_GRAY16NORM_PALETTE_PRISM:   lut = gray16_to_rgb_prism; break;
        default:                              lut = gray16_to_rgb; break;
      }

      if (G_LIKELY (!self->auto_range && minPixelValue == 0 && maxPixelValue == 65535)) {
        /* Full-range manual mapping: index == v */
        for (gsize y = 0; y < height; y++) {
          const guint8 *in_line = in_base + y * in_stride;
          guint8 *out_line = out_base + y * out_stride;
          for (gsize x = 0; x < width; x++) {
            const guint16 v = GST_READ_UINT16_LE (in_line + (x << 1));
            const uint8_t *rgb = lut[v];
            const gsize off = 3 * x;
            out_line[off + 0] = rgb[0];
            out_line[off + 1] = rgb[1];
            out_line[off + 2] = rgb[2];
          }
        }
        return GST_FLOW_OK;
      }

      /* General case: compute normalized index in [0..65535] with rounding */
      const guint32 scale = 65535u; /* numerator in (t * scale + range/2) / range */
      for (gsize y = 0; y < height; y++) {
        const guint8 *in_line = in_base + y * in_stride;
        guint8 *out_line = out_base + y * out_stride;
        for (gsize x = 0; x < width; x++) {
          const guint16 v = GST_READ_UINT16_LE (in_line + (x << 1));
          guint32 idx;
          if (G_UNLIKELY (v <= minPixelValue)) {
            idx = 0u;
          } else if (G_UNLIKELY (v >= maxPixelValue)) {
            idx = 65535u;
          } else {
            const guint32 t = (guint32) (v - minPixelValue);
            idx = (t * scale + (range >> 1)) / range; /* rounded */
            if (idx > 65535u) idx = 65535u; /* safety */
          }
          const uint8_t *rgb = lut[idx];
          const gsize off = 3 * x;
          out_line[off + 0] = rgb[0];
          out_line[off + 1] = rgb[1];
          out_line[off + 2] = rgb[2];
        }
      }
      return GST_FLOW_OK;
    }

    /* Below: GRAY8 output path (original behavior) */
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

#if GST_GRAY16NORM_HAVE_NEON && defined(__aarch64__)
    /* NEON path: use Q8 factor: val = ((t * scale_q8) + 128) >> 8; then narrow/saturate */
    const guint32 N = 255u << 8; /* 65280 */
    const guint16 scale_q8 = (guint16) ((N + (range >> 1)) / range);
    const uint16x8_t vmin_dup = vdupq_n_u16 ((uint16_t) minPixelValue);
    const uint16x8_t vscale_dup = vdupq_n_u16 (scale_q8);
    const uint16x8_t vrange_dup = vdupq_n_u16 ((uint16_t) range);
    const uint32x4_t vround_dup = vdupq_n_u32 (128u);
    GST_LOG_OBJECT (self, "normalize: NEON scale_q8=%u (range=%u)",
                    (unsigned) scale_q8, (unsigned) range);

    for (gsize y = 0; y < height; y++) {
      const guint8 *in_line_u8 = in_base + y * in_stride;
      guint8 *out_line = out_base + y * out_stride;
      const guint16 *in_line = (const guint16 *) in_line_u8;
      gsize x = 0;
      const gsize w8 = width & ~(gsize)7;
      for (; x < w8; x += 8) {
        uint16x8_t vin = vld1q_u16 (in_line + x);
        /* t = clamp(v - min, 0..range) */
        uint16x8_t t = vqsubq_u16 (vin, vmin_dup);
        t = vminq_u16 (t, vrange_dup);
        /* 16x16 -> 32 */
        uint32x4_t lo = vmull_u16 (vget_low_u16 (t), vget_low_u16 (vscale_dup));
        uint32x4_t hi = vmull_u16 (vget_high_u16 (t), vget_high_u16 (vscale_dup));
        lo = vaddq_u32 (lo, vround_dup);
        hi = vaddq_u32 (hi, vround_dup);
        uint16x4_t lo16 = vshrn_n_u32 (lo, 8);
        uint16x4_t hi16 = vshrn_n_u32 (hi, 8);
        uint16x8_t packed16 = vcombine_u16 (lo16, hi16);
        /* narrow with saturation to [0..255] */
        uint8x8_t packed8 = vqmovn_u16 (packed16);
        vst1_u8 (out_line + x, packed8);
      }
      /* tail */
      for (; x < width; x++) {
        const guint16 v = in_line[x];
        if (v <= minPixelValue) {
          out_line[x] = 0;
        } else if (v >= maxPixelValue) {
          out_line[x] = 255;
        } else {
          const guint32 t = (guint32) (v - minPixelValue);
          const guint32 val = (t * (guint32) scale_q8 + 128u) >> 8;
          out_line[x] = (val > 255u) ? 255u : (guint8) val;
        }
      }
    }
#elif GST_GRAY16NORM_HAVE_SSE2
    /* SSE2 path: Q16 scaling with pmulhuw, clamp t in [0..range] using unsigned saturating ops */
    const guint16 scale_q16 = (guint16) (((255u << 16) + (range >> 1)) / range);
    const __m128i vmin_dup = _mm_set1_epi16 ((short) minPixelValue);
    const __m128i vrange_dup = _mm_set1_epi16 ((short) range);
    const __m128i vscale_dup = _mm_set1_epi16 ((short) scale_q16);
    const __m128i vzero = _mm_setzero_si128 ();
    GST_LOG_OBJECT (self, "normalize: SSE2 scale_q16=%u (range=%u)",
                    (unsigned) scale_q16, (unsigned) range);

    for (gsize y = 0; y < height; y++) {
      const guint8 *in_line_u8 = in_base + y * in_stride;
      guint8 *out_line = out_base + y * out_stride;
      const guint16 *in_line = (const guint16 *) in_line_u8;
      gsize x = 0;
      const gsize w8 = width & ~(gsize)7;
      for (; x < w8; x += 8) {
        __m128i vin = _mm_loadu_si128 ((const __m128i *) (in_line + x));
        /* t = clamp(v - min, 0..range) */
        __m128i t = _mm_subs_epu16 (vin, vmin_dup);              /* saturating unsigned subtract */
        __m128i over = _mm_subs_epu16 (t, vrange_dup);            /* over = max(t - range, 0) */
        t = _mm_sub_epi16 (t, over);                              /* t = min(t, range) */
        /* Multiply by scale_q16 and keep high 16 bits (>> 16) */
        __m128i prod_hi = _mm_mulhi_epu16 (t, vscale_dup);        /* unsigned high half */
        /* Narrow to 8-bit with saturation (values are 0..255 already) */
        __m128i bytes = _mm_packus_epi16 (prod_hi, vzero);
        _mm_storel_epi64 ((__m128i *) (out_line + x), bytes);     /* store 8 bytes */
      }
      /* tail */
      for (; x < width; x++) {
        const guint16 v = in_line[x];
        if (v <= minPixelValue) {
          out_line[x] = 0;
        } else if (v >= maxPixelValue) {
          out_line[x] = 255;
        } else {
          const guint32 t = (guint32) (v - minPixelValue);
          const guint32 val = (t * (guint32) scale_q16) >> 16; /* already rounded in scale */
          out_line[x] = (val > 255u) ? 255u : (guint8) val;
        }
      }
    }
#else
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
#endif

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
		if (direction == GST_PAD_SINK) {
			/* From sink (GRAY16_LE) to src: advertise both GRAY8 and RGB */
			GstStructure *s_gray8 = gst_structure_copy (s);
			gst_structure_set (s_gray8, "format", G_TYPE_STRING, "GRAY8", NULL);
			gst_caps_append_structure (result, s_gray8);

			GstStructure *s_rgb = gst_structure_copy (s);
			gst_structure_set (s_rgb, "format", G_TYPE_STRING, "RGB", NULL);
			gst_caps_append_structure (result, s_rgb);
		} else {
			/* From src caps to sink caps */
			GstStructure *s2 = gst_structure_copy (s);
			gst_structure_set (s2, "format", G_TYPE_STRING, "GRAY16_LE", NULL);
			gst_caps_append_structure (result, s2);
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


/* --- GObject properties --- */

enum {
	PROP_0,
	PROP_AUTO_RANGE,
	PROP_BLACK_LEVEL,
	PROP_WHITE_LEVEL,
	PROP_PALETTE,
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
		case PROP_PALETTE: {
			const gchar *s = g_value_get_string (value);
			if (!s) { self->palette = GST_GRAY16NORM_PALETTE_TURBO; break; }
			if (g_ascii_strcasecmp (s, "turbo") == 0) {
				self->palette = GST_GRAY16NORM_PALETTE_TURBO;
			} else if (g_ascii_strcasecmp (s, "viridis") == 0 ||
			           g_ascii_strcasecmp (s, "virdis") == 0) {
				self->palette = GST_GRAY16NORM_PALETTE_VIRIDIS;
			} else if (g_ascii_strcasecmp (s, "magma") == 0) {
				self->palette = GST_GRAY16NORM_PALETTE_MAGMA;
			} else if (g_ascii_strcasecmp (s, "jet") == 0) {
				self->palette = GST_GRAY16NORM_PALETTE_JET;
			} else if (g_ascii_strcasecmp (s, "prism") == 0) {
				self->palette = GST_GRAY16NORM_PALETTE_PRISM;
			} else {
				GST_WARNING_OBJECT (self, "Unknown palette '%s', using turbo", s);
				self->palette = GST_GRAY16NORM_PALETTE_TURBO;
			}
			break; }
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
		case PROP_PALETTE: {
			const gchar *name = "turbo";
			switch (self->palette) {
				case GST_GRAY16NORM_PALETTE_TURBO: name = "turbo"; break;
				case GST_GRAY16NORM_PALETTE_VIRIDIS: name = "viridis"; break;
				case GST_GRAY16NORM_PALETTE_MAGMA: name = "magma"; break;
				case GST_GRAY16NORM_PALETTE_JET: name = "jet"; break;
				case GST_GRAY16NORM_PALETTE_PRISM: name = "prism"; break;
			}
			g_value_set_string (value, name);
			break; }
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

	/* Palette for RGB output (ignored when output is GRAY8) */
	g_object_class_install_property (
			gobject_class, PROP_PALETTE,
			g_param_spec_string ("palette", "LUT palette for RGB",
				"When output is RGB, select LUT palette: turbo (default), viridis (alias: virdis), magma, jet, prism",
				"turbo",
				G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

	gst_element_class_set_static_metadata (element_class,
			"Gray16 normalizer", "Filter/Effect/Video",
			"Normalize GRAY16 to GRAY8 or map to RGB via LUT; auto or manual range",
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
			"GRAY16 normalizer to GRAY8 or RGB (LUT)");
}

static void
gst_gray16norm_init (GstGray16Norm * self)
{
	self->auto_range  = TRUE;
	self->black_level = 0;
	self->white_level = 65535;
	self->palette     = GST_GRAY16NORM_PALETTE_TURBO;
}

/* plugin registration moved to gstgray16plugin.c */
