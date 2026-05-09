/* vlx_metadata.c — GStreamer pbutils discoverer-based metadata reader
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "media/metadata.h"
#include "utils/thread_pool.h"
#include "utils/log.h"

#include <gst/gst.h>
#include <gst/pbutils/pbutils.h>
#include <string.h>

typedef struct {
    gchar              *uri;
    VlxMetadataReadyCb  callback;
    gpointer            user_data;
    VlxMediaInfo       *result;
} MetaRequest;

/* ── VlxMediaInfo lifecycle ──────────────────────────────────────────── */
void
vlx_media_info_free (VlxMediaInfo *info)
{
    if (!info) return;
    g_free (info->title);
    g_free (info->artist);
    g_free (info->album);
    g_free (info->genre);
    g_free (info->codec_video);
    g_free (info->codec_audio);
    g_slice_free (VlxMediaInfo, info);
}

/* ── Tag extraction helpers ──────────────────────────────────────────── */
static gchar *
tag_string (const GstTagList *tags, const gchar *tag)
{
    gchar *val = NULL;
    gst_tag_list_get_string (tags, tag, &val);
    return val;
}

/* ── Worker ──────────────────────────────────────────────────────────── */
static gpointer
meta_worker (gpointer data)
{
    MetaRequest *req  = data;
    VlxMediaInfo *info = g_slice_new0 (VlxMediaInfo);
    req->result = info;

    GError        *err  = NULL;
    GstDiscoverer *disc = gst_discoverer_new (10 * GST_SECOND, &err);
    if (!disc) {
        VLX_LOG_ERROR (VLX_LOG_DOMAIN_MEDIA,
                       "GstDiscoverer init failed: %s", err->message);
        g_error_free (err);
        return NULL;
    }

    GstDiscovererInfo *dinfo = gst_discoverer_discover_uri (disc, req->uri, &err);
    gst_object_unref (disc);

    if (!dinfo) {
        if (err) {
            VLX_LOG_ERROR (VLX_LOG_DOMAIN_MEDIA,
                           "Discover failed for %s: %s", req->uri, err->message);
            g_error_free (err);
        }
        return NULL;
    }

    /* Duration */
    info->duration_us =
        (gint64) gst_discoverer_info_get_duration (dinfo) / GST_USECOND;

    /* Tags */
    const GstTagList *tags = gst_discoverer_info_get_tags (dinfo);
    if (tags) {
        info->title  = tag_string (tags, GST_TAG_TITLE);
        info->artist = tag_string (tags, GST_TAG_ARTIST);
        info->album  = tag_string (tags, GST_TAG_ALBUM);
        info->genre  = tag_string (tags, GST_TAG_GENRE);
        guint year   = 0;
        gst_tag_list_get_uint (tags, GST_TAG_DATE, &year);
        info->year = (gint) year;
        gst_tag_list_unref ((GstTagList *) tags);
    }

    /* Stream info */
    GList *streams = gst_discoverer_info_get_stream_list (dinfo);
    for (GList *l = streams; l; l = l->next) {
        GstDiscovererStreamInfo *si = l->data;

        if (GST_IS_DISCOVERER_VIDEO_INFO (si)) {
            GstDiscovererVideoInfo *vi = (GstDiscovererVideoInfo *) si;
            info->width      = (gint) gst_discoverer_video_info_get_width (vi);
            info->height     = (gint) gst_discoverer_video_info_get_height (vi);
            guint num = gst_discoverer_video_info_get_framerate_num (vi);
            guint den = gst_discoverer_video_info_get_framerate_denom (vi);
            info->frame_rate = den > 0 ? (gdouble) num / den : 0.0;

            GstCaps *caps = gst_discoverer_stream_info_get_caps (si);
            if (caps) {
                info->codec_video = gst_pb_utils_get_codec_description (caps);
                gst_caps_unref (caps);
            }
        } else if (GST_IS_DISCOVERER_AUDIO_INFO (si)) {
            GstCaps *caps = gst_discoverer_stream_info_get_caps (si);
            if (caps) {
                if (!info->codec_audio)
                    info->codec_audio =
                        gst_pb_utils_get_codec_description (caps);
                gst_caps_unref (caps);
            }
        }
    }
    gst_discoverer_stream_info_list_free (streams);
    gst_discoverer_info_unref (dinfo);

    VLX_LOG_INFO (VLX_LOG_DOMAIN_MEDIA,
                  "Metadata ready: \"%s\" %dx%d %.2ffps dur=%" G_GINT64_FORMAT "µs",
                  info->title ? info->title : "(no title)",
                  info->width, info->height, info->frame_rate,
                  info->duration_us);
    return NULL;
}

static void
meta_complete (gpointer result, gpointer user_data)
{
    MetaRequest *req = user_data;
    (void) result;

    if (req->callback)
        req->callback (req->uri, req->result, req->user_data);
    /* Caller owns req->result; do NOT free here */

    g_free (req->uri);
    g_slice_free (MetaRequest, req);
}

void
vlx_metadata_read_async (const gchar        *uri,
                          VlxMetadataReadyCb  callback,
                          gpointer            user_data)
{
    g_return_if_fail (uri != NULL);

    MetaRequest *req  = g_slice_new0 (MetaRequest);
    req->uri          = g_strdup (uri);
    req->callback     = callback;
    req->user_data    = user_data;

    vlx_thread_pool_push (meta_worker, meta_complete, NULL, req, NULL);
}
