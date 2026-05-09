/* vlx_hwaccel.c — Hardware acceleration detection
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "pipeline/hwaccel.h"
#include "utils/log.h"

VlxHwAccelType
vlx_hwaccel_detect (void)
{
    /* Check for VAAPI (Intel/AMD) */
    GstElementFactory *vaapi = gst_element_factory_find ("vaapidecodebin");
    if (vaapi) {
        gst_object_unref (vaapi);
        VLX_LOG_INFO (VLX_LOG_DOMAIN_PIPELINE, "VAAPI decoder available");
        return VLX_HWACCEL_VAAPI;
    }

    /* Check for NVDEC (NVIDIA) */
    GstElementFactory *nvdec = gst_element_factory_find ("nvh264dec");
    if (nvdec) {
        gst_object_unref (nvdec);
        VLX_LOG_INFO (VLX_LOG_DOMAIN_PIPELINE, "NVDEC decoder available");
        return VLX_HWACCEL_NVDEC;
    }

    /* Check for V4L2 (e.g. Raspberry Pi) */
    GstElementFactory *v4l2 = gst_element_factory_find ("v4l2h264dec");
    if (v4l2) {
        gst_object_unref (v4l2);
        VLX_LOG_INFO (VLX_LOG_DOMAIN_PIPELINE, "V4L2 decoder available");
        return VLX_HWACCEL_V4L2;
    }

    VLX_LOG_INFO (VLX_LOG_DOMAIN_PIPELINE,
                  "No hardware decoder found — using software decode");
    return VLX_HWACCEL_NONE;
}

const gchar *
vlx_hwaccel_type_to_string (VlxHwAccelType type)
{
    switch (type) {
    case VLX_HWACCEL_VAAPI: return "VAAPI";
    case VLX_HWACCEL_NVDEC: return "NVDEC";
    case VLX_HWACCEL_V4L2:  return "V4L2";
    case VLX_HWACCEL_NONE:  return "Software";
    default:                return "Unknown";
    }
}

GstElement *
vlx_hwaccel_create_decoder (VlxHwAccelType type)
{
    switch (type) {
    case VLX_HWACCEL_VAAPI:
        return gst_element_factory_make ("vaapidecodebin", "hwdec");
    case VLX_HWACCEL_NVDEC:
        return gst_element_factory_make ("nvh264dec", "hwdec");
    case VLX_HWACCEL_V4L2:
        return gst_element_factory_make ("v4l2h264dec", "hwdec");
    case VLX_HWACCEL_NONE:
    default:
        return NULL;
    }
}
