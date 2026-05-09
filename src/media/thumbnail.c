/* vlx_thumbnail.c — Async LRU thumbnail cache (512 entries, disk-backed)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Strategy:
 *  1. Check in-memory VlxCache (LRU, 512 entries).
 *  2. Check XDG_CACHE_HOME/velox/thumbs/<hash>.jpg on disk.
 *  3. If absent, spawn a GStreamer pipeline (playbin, seek, videoframe-step)
 *     in a GThreadPool worker to extract a JPEG frame, then notify the
 *     caller on the main thread via g_main_context_invoke.
 *
 * Hash key: SHA-256(uri + mtime) rendered as hex, truncated to 16 chars.
 */

#include "media/thumbnail.h"
#include "utils/cache.h"
#include "utils/thread_pool.h"
#include "utils/log.h"

#include <gdk/gdk.h>
#include <gst/gst.h>
#include <gst/pbutils/pbutils.h>
#include <gio/gio.h>

#define CACHE_CAPACITY 512
#define THUMB_WIDTH    256
#define THUMB_HEIGHT   144

/* ── Internal request descriptor ────────────────────────────────────── */
typedef struct {
    gchar              *uri;
    gchar              *mem_key;   /* uri@position_us */
    gint64              position_us;
    VlxThumbnailReadyCb callback;
    gpointer            user_data;
    VlxThumbnailCache  *cache_ref;
    GdkTexture         *result;    /* filled by worker */
} ThumbRequest;

struct _VlxThumbnailCache {
    GObject    parent_instance;
    VlxCache  *mem_cache;    /* uri → GdkTexture* */
    gchar     *disk_dir;     /* XDG cache dir for JPEG files */
};

G_DEFINE_TYPE (VlxThumbnailCache, vlx_thumbnail_cache, G_TYPE_OBJECT)

/* ── Disk helpers ────────────────────────────────────────────────────── */
static gchar *
cache_key (const gchar *uri, gint64 position_us)
{
    gchar       *combined = g_strdup_printf ("%s@%" G_GINT64_FORMAT, uri, position_us);
    GChecksum   *cs       = g_checksum_new (G_CHECKSUM_SHA256);
    g_checksum_update (cs, (const guchar *) combined, -1);
    const gchar *hex = g_checksum_get_string (cs);
    gchar       *key = g_strndup (hex, 16);
    g_checksum_free (cs);
    g_free (combined);
    return key;
}

static gchar *
disk_path (VlxThumbnailCache *self, const gchar *uri, gint64 position_us)
{
    gchar *key  = cache_key (uri, position_us);
    gchar *path = g_build_filename (self->disk_dir, key, NULL);
    g_free (key);
    gchar *full = g_strconcat (path, ".png", NULL);
    g_free (path);
    return full;
}

static GdkTexture *
load_from_disk (const gchar *path)
{
    GError     *err  = NULL;
    GdkTexture *tex  = gdk_texture_new_from_filename (path, &err);
    if (err) {
        g_error_free (err);
        return NULL;
    }
    return tex;
}

/* ── Worker thread: extract frame with GStreamer ─────────────────────── */
static gpointer
thumb_worker (gpointer data)
{
    ThumbRequest *req = data;

    /* Use GstDiscoverer just to confirm the file is valid */
    GstDiscoverer *disc = gst_discoverer_new (5 * GST_SECOND, NULL);
    if (!disc) goto done;

    GstDiscovererInfo *info =
        gst_discoverer_discover_uri (disc, req->uri, NULL);
    gst_object_unref (disc);
    if (!info) goto done;
    gst_discoverer_info_unref (info);

    /* Build a minimal pipeline: uridecodebin → videoconvert → appsink */
    gchar *pipeline_str = g_strdup_printf (
        "uridecodebin uri=\"%s\" ! videoconvert ! "
        "videoscale ! video/x-raw,width=%d,height=%d ! "
        "pngenc ! appsink name=sink max-buffers=1 drop=true",
        req->uri, THUMB_WIDTH, THUMB_HEIGHT);

    GError     *err      = NULL;
    GstElement *pipeline = gst_parse_launch (pipeline_str, &err);
    g_free (pipeline_str);
    if (err) { g_error_free (err); goto done; }

    /* Seek to the requested position */
    gst_element_set_state (pipeline, GST_STATE_PAUSED);
    gst_element_get_state (pipeline, NULL, NULL, 5 * GST_SECOND);

    if (req->position_us > 0) {
        gst_element_seek_simple (pipeline,
                                 GST_FORMAT_TIME,
                                 GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT,
                                 req->position_us * GST_USECOND);
        gst_element_get_state (pipeline, NULL, NULL, 3 * GST_SECOND);
    }

    /* Pull one sample */
    GstElement *sink = gst_bin_get_by_name (GST_BIN (pipeline), "sink");
    GstSample  *sample = NULL;
    g_signal_emit_by_name (sink, "pull-preroll", &sample);
    gst_object_unref (sink);

    gst_element_set_state (pipeline, GST_STATE_NULL);
    gst_object_unref (pipeline);

    if (!sample) goto done;

    GstBuffer *buf   = gst_sample_get_buffer (sample);
    GstMapInfo map;
    if (gst_buffer_map (buf, &map, GST_MAP_READ)) {
        GBytes     *bytes = g_bytes_new (map.data, map.size);
        GdkTexture *tex   = gdk_texture_new_from_bytes (bytes, NULL);
        g_bytes_unref (bytes);
        req->result = tex;
        gst_buffer_unmap (buf, &map);
    }
    gst_sample_unref (sample);

done:
    return NULL;
}

/* ── Completion (main thread) ────────────────────────────────────────── */
static void
thumb_complete (gpointer result, gpointer user_data)
{
    ThumbRequest *req = user_data;
    (void) result;

    if (req->result) {
        /* Store in memory cache keyed by uri@position */
        vlx_cache_insert (req->cache_ref->mem_cache,
                          g_strdup (req->mem_key),
                          g_object_ref (req->result));

        /* Persist to disk */
        gchar *path = disk_path (req->cache_ref, req->uri, req->position_us);
        GError *err = NULL;
        GdkTexture *tex = req->result;
        GBytes *bytes = gdk_texture_save_to_png_bytes (tex);
        if (bytes) {
            g_file_set_contents (path,
                                 g_bytes_get_data (bytes, NULL),
                                 g_bytes_get_size (bytes),
                                 &err);
            if (err) g_error_free (err);
            g_bytes_unref (bytes);
        }
        g_free (path);
    }

    if (req->callback)
        req->callback (req->uri, req->result, req->user_data);

    g_clear_object (&req->result);
    g_object_unref (req->cache_ref);
    g_free (req->uri);
    g_free (req->mem_key);
    g_slice_free (ThumbRequest, req);
}

/* ── GObject boilerplate ─────────────────────────────────────────────── */
static void
vlx_thumbnail_cache_finalize (GObject *obj)
{
    VlxThumbnailCache *self = VLX_THUMBNAIL_CACHE (obj);
    vlx_cache_free (self->mem_cache);
    g_free (self->disk_dir);
    G_OBJECT_CLASS (vlx_thumbnail_cache_parent_class)->finalize (obj);
}

static void
vlx_thumbnail_cache_class_init (VlxThumbnailCacheClass *klass)
{
    G_OBJECT_CLASS (klass)->finalize = vlx_thumbnail_cache_finalize;
}

static void
vlx_thumbnail_cache_init (VlxThumbnailCache *self)
{
    self->mem_cache = vlx_cache_new (CACHE_CAPACITY,
                                     g_free,
                                     g_object_unref);

    self->disk_dir = g_build_filename (g_get_user_cache_dir (),
                                       "velox", "thumbs", NULL);
    g_mkdir_with_parents (self->disk_dir, 0700);
}

VlxThumbnailCache *
vlx_thumbnail_cache_get_default (void)
{
    static VlxThumbnailCache *instance = NULL;
    if (g_once_init_enter_pointer (&instance)) {
        VlxThumbnailCache *c = g_object_new (VLX_TYPE_THUMBNAIL_CACHE, NULL);
        g_once_init_leave_pointer (&instance, c);
    }
    return instance;
}

void
vlx_thumbnail_cache_request (VlxThumbnailCache  *cache,
                              const gchar        *uri,
                              gint64              position_us,
                              VlxThumbnailReadyCb callback,
                              gpointer            user_data)
{
    g_return_if_fail (VLX_IS_THUMBNAIL_CACHE (cache));

    /* 1. Memory cache hit — keyed by uri@position */
    gchar *mem_key = g_strdup_printf ("%s@%" G_GINT64_FORMAT, uri, position_us);
    GdkTexture *tex = vlx_cache_lookup (cache->mem_cache, mem_key);
    if (tex) {
        g_free (mem_key);
        if (callback) callback (uri, tex, user_data);
        return;
    }

    /* 2. Disk cache hit */
    gchar *path = disk_path (cache, uri, position_us);
    tex = load_from_disk (path);
    g_free (path);
    if (tex) {
        vlx_cache_insert (cache->mem_cache,
                          g_strdup (mem_key), g_object_ref (tex));
        g_free (mem_key);
        if (callback) callback (uri, tex, user_data);
        g_object_unref (tex);
        return;
    }

    /* 3. Schedule background extraction */
    ThumbRequest *req  = g_slice_new0 (ThumbRequest);
    req->uri           = g_strdup (uri);
    req->mem_key       = mem_key;  /* transferred */
    req->position_us   = position_us;
    req->callback      = callback;
    req->user_data     = user_data;
    req->cache_ref     = g_object_ref (cache);

    vlx_thread_pool_push (thumb_worker, thumb_complete,
                          req, req, NULL);
}

GdkTexture *
vlx_thumbnail_cache_lookup (VlxThumbnailCache *cache,
                             const gchar       *uri)
{
    g_return_val_if_fail (VLX_IS_THUMBNAIL_CACHE (cache), NULL);
    return vlx_cache_lookup (cache->mem_cache, uri);
}
