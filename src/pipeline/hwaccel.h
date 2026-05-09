/* vlx_hwaccel.h — Hardware acceleration detection (VAAPI / NVDEC)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <glib.h>
#include <gst/gst.h>

G_BEGIN_DECLS

typedef enum {
    VLX_HWACCEL_NONE,
    VLX_HWACCEL_VAAPI,
    VLX_HWACCEL_NVDEC,
    VLX_HWACCEL_V4L2,
} VlxHwAccelType;

VlxHwAccelType vlx_hwaccel_detect       (void);
const gchar   *vlx_hwaccel_type_to_string (VlxHwAccelType type);
GstElement    *vlx_hwaccel_create_decoder (VlxHwAccelType type);

G_END_DECLS
