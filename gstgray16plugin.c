// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * Plugin registration unit
 *
 * Registers all elements that comprise this plugin. Moved out of
 * gstgray16norm.c per project request to keep element code separate
 * from plugin boilerplate.
 */

#include <gst/gst.h>

/* Some environments expect PACKAGE to be defined for GST_PLUGIN_DEFINE */
#ifndef PACKAGE
#define PACKAGE "gray16norm"
#endif

/* Elements implemented in other translation units */
extern GType gst_gray16norm_get_type (void);

static gboolean
plugin_init (GstPlugin * plugin)
{
  gboolean ok = TRUE;

  ok &= gst_element_register (plugin, "gray16norm", GST_RANK_NONE,
                              gst_gray16norm_get_type());
  return ok;
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
