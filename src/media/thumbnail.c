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
    GBytes             *result_bytes;
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

static GThread     *worker_thread = NULL;
static GAsyncQueue *worker_queue  = NULL;

static gboolean
thumb_complete_idle (gpointer data)
{
    ThumbRequest *req = data;

    GdkTexture *tex = NULL;
    if (req->result_bytes) {
        tex = gdk_texture_new_from_bytes (req->result_bytes, NULL);
        g_bytes_unref (req->result_bytes);
    }

    if (tex) {
        /* Store in memory cache keyed by uri@position */
        vlx_cache_insert (req->cache_ref->mem_cache,
                          g_strdup (req->mem_key),
                          g_object_ref (tex));

        /* Persist to disk */
        gchar *path = disk_path (req->cache_ref, req->uri, req->position_us);
        GError *err = NULL;
        GBytes *png_bytes = gdk_texture_save_to_png_bytes (tex);
        if (png_bytes) {
            g_file_set_contents (path,
                                 g_bytes_get_data (png_bytes, NULL),
                                 g_bytes_get_size (png_bytes),
                                 &err);
            if (err) g_error_free (err);
            g_bytes_unref (png_bytes);
        }
        g_free (path);
    }

    if (req->callback)
        req->callback (req->uri, req->position_us, tex, req->user_data);

    g_clear_object (&tex);
    g_object_unref (req->cache_ref);
    g_free (req->uri);
    g_free (req->mem_key);
    g_slice_free (ThumbRequest, req);

    return G_SOURCE_REMOVE;
}

static gpointer
thumb_worker_loop (gpointer data)
{
    (void) data;
    GError *err = NULL;
    GstElement *pipeline = gst_parse_launch (
        "uridecodebin name=src ! videoconvert ! "
        "videoscale ! video/x-raw,width=256,height=144 ! "
        "pngenc ! appsink name=sink max-buffers=1 drop=true", &err);
    
    if (err) {
        g_printerr ("Thumbnail pipeline error: %s\n", err->message);
        g_error_free (err);
        return NULL;
    }

    GstElement *src  = gst_bin_get_by_name (GST_BIN (pipeline), "src");
    GstElement *sink = gst_bin_get_by_name (GST_BIN (pipeline), "sink");
    gchar *current_uri = NULL;

    while (TRUE) {
        ThumbRequest *req = g_async_queue_pop (worker_queue);
        if (!req) continue;
        
        if (!req->uri) {
            g_slice_free (ThumbRequest, req);
            break; /* Poison pill */
        }

        /* Change URI if needed */
        if (g_strcmp0 (current_uri, req->uri) != 0) {
            gst_element_set_state (pipeline, GST_STATE_NULL);
            g_object_set (src, "uri", req->uri, NULL);
            g_free (current_uri);
            current_uri = g_strdup (req->uri);
            
            gst_element_set_state (pipeline, GST_STATE_PAUSED);
        }

        /* Wait for pipeline to be ready. If it fails (e.g. invalid file), skip seek. */
        GstStateChangeReturn ret = gst_element_get_state (pipeline, NULL, NULL, 5 * GST_SECOND);
        if (ret == GST_STATE_CHANGE_FAILURE) {
            g_main_context_invoke (NULL, thumb_complete_idle, req);
            continue;
        }

        /* Seek to the requested position */
        if (req->position_us >= 0) {
            gst_element_seek_simple (pipeline,
                                     GST_FORMAT_TIME,
                                     GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT,
                                     req->position_us * GST_USECOND);
            gst_element_get_state (pipeline, NULL, NULL, 2 * GST_SECOND);
        }

        /* Pull one sample with timeout */
        GstSample *sample = NULL;
        g_signal_emit_by_name (sink, "try-pull-preroll", (guint64)(1 * GST_SECOND), &sample);

        if (sample) {
            GstBuffer *buf = gst_sample_get_buffer (sample);
            GstMapInfo map;
            if (gst_buffer_map (buf, &map, GST_MAP_READ)) {
                req->result_bytes = g_bytes_new (map.data, map.size);
                gst_buffer_unmap (buf, &map);
            }
            gst_sample_unref (sample);
        }

        /* Send result to main thread */
        g_main_context_invoke (NULL, thumb_complete_idle, req);
    }

    gst_element_set_state (pipeline, GST_STATE_NULL);
    gst_object_unref (src);
    gst_object_unref (sink);
    gst_object_unref (pipeline);
    g_free (current_uri);

    return NULL;
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
        if (callback) callback (uri, position_us, tex, user_data);
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
        if (callback) callback (uri, position_us, tex, user_data);
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

    static gsize init = 0;
    if (g_once_init_enter (&init)) {
        worker_queue = g_async_queue_new ();
        worker_thread = g_thread_new ("thumb-worker", thumb_worker_loop, NULL);
        g_once_init_leave (&init, 1);
    }

    g_async_queue_push (worker_queue, req);
}

GdkTexture *
vlx_thumbnail_cache_lookup (VlxThumbnailCache *cache,
                             const gchar       *uri)
{
    g_return_val_if_fail (VLX_IS_THUMBNAIL_CACHE (cache), NULL);
    return vlx_cache_lookup (cache->mem_cache, uri);
}

void
vlx_thumbnail_cache_shutdown (void)
{
    if (!worker_queue || !worker_thread)
        return;

    /* Send poison pill: a request with NULL uri causes the worker to exit */
    ThumbRequest *poison = g_slice_new0 (ThumbRequest);
    poison->uri = NULL;
    g_async_queue_push (worker_queue, poison);

    g_thread_join (worker_thread);
    worker_thread = NULL;

    g_async_queue_unref (worker_queue);
    worker_queue = NULL;
}
