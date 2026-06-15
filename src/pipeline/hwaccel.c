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

