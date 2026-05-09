/* vlx_discovery.c — GFileMonitor directory watcher
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Watches one or more directories for media file additions/removals and
 * keeps a VlxPlaylist in sync.  Uses GFileMonitor (inotify on Linux).
 */

#include "media/discovery.h"
#include "utils/log.h"

#include <gio/gio.h>
#include <string.h>

/* Recognised media extensions */
static const gchar *MEDIA_EXTS[] = {
    "mkv", "mp4", "avi", "mov", "wmv", "flv", "webm",
    "m4v", "ts",  "m2ts","ogv", "mp3", "flac","ogg",
    "aac", "wav", "opus", NULL,
};

static gboolean
is_media_file (const gchar *name)
{
    const gchar *dot = strrchr (name, '.');
    if (!dot) return FALSE;
    const gchar *ext = dot + 1;
    for (gint i = 0; MEDIA_EXTS[i]; i++) {
        if (g_ascii_strcasecmp (ext, MEDIA_EXTS[i]) == 0)
            return TRUE;
    }
    return FALSE;
}

typedef struct {
    GFileMonitor *monitor;
    gchar        *directory;
} WatchEntry;

struct _VlxMediaDiscovery {
    GObject      parent_instance;
    VlxPlaylist *playlist;
    GList       *watches;   /* WatchEntry* */
};

G_DEFINE_TYPE (VlxMediaDiscovery, vlx_media_discovery, G_TYPE_OBJECT)

static void
on_changed (GFileMonitor      *monitor,
            GFile             *file,
            GFile             *other_file,
            GFileMonitorEvent  event,
            gpointer           data)
{
    VlxMediaDiscovery *self = VLX_MEDIA_DISCOVERY (data);
    (void) monitor; (void) other_file;

    gchar *name = g_file_get_basename (file);
    if (!is_media_file (name)) {
        g_free (name);
        return;
    }

    gchar *uri = g_file_get_uri (file);

    if (event == G_FILE_MONITOR_EVENT_CREATED) {
        VLX_LOG_INFO (VLX_LOG_DOMAIN_MEDIA, "Discovered: %s", uri);
        vlx_playlist_append (self->playlist, uri);
    } else if (event == G_FILE_MONITOR_EVENT_DELETED) {
        /* Find and remove from playlist */
        for (guint i = 0; i < vlx_playlist_get_length (self->playlist); i++) {
            if (g_strcmp0 (vlx_playlist_get_uri_at (self->playlist, i), uri) == 0) {
                vlx_playlist_remove (self->playlist, i);
                VLX_LOG_INFO (VLX_LOG_DOMAIN_MEDIA, "Removed: %s", uri);
                break;
            }
        }
    }

    g_free (name);
    g_free (uri);
}

static void
scan_directory_initial (VlxMediaDiscovery *self, const gchar *directory)
{
    GFile      *dir  = g_file_new_for_path (directory);
    GError     *err  = NULL;
    GFileEnumerator *en = g_file_enumerate_children (
        dir,
        G_FILE_ATTRIBUTE_STANDARD_NAME,
        G_FILE_QUERY_INFO_NONE,
        NULL, &err);

    if (!en) {
        VLX_LOG_WARNING (VLX_LOG_DOMAIN_MEDIA,
                         "Cannot scan directory %s: %s",
                         directory, err->message);
        g_error_free (err);
        g_object_unref (dir);
        return;
    }

    GFileInfo *fi;
    while ((fi = g_file_enumerator_next_file (en, NULL, NULL)) != NULL) {
        const gchar *name = g_file_info_get_name (fi);
        if (is_media_file (name)) {
            GFile *child = g_file_get_child (dir, name);
            gchar *uri   = g_file_get_uri (child);
            vlx_playlist_append (self->playlist, uri);
            g_free (uri);
            g_object_unref (child);
        }
        g_object_unref (fi);
    }

    g_object_unref (en);
    g_object_unref (dir);
}

static void
vlx_media_discovery_dispose (GObject *obj)
{
    VlxMediaDiscovery *self = VLX_MEDIA_DISCOVERY (obj);

    for (GList *l = self->watches; l; l = l->next) {
        WatchEntry *w = l->data;
        g_file_monitor_cancel (w->monitor);
        g_object_unref (w->monitor);
        g_free (w->directory);
        g_slice_free (WatchEntry, w);
    }
    g_list_free (self->watches);
    self->watches = NULL;

    g_clear_object (&self->playlist);
    G_OBJECT_CLASS (vlx_media_discovery_parent_class)->dispose (obj);
}

static void
vlx_media_discovery_class_init (VlxMediaDiscoveryClass *klass)
{
    G_OBJECT_CLASS (klass)->dispose = vlx_media_discovery_dispose;
}

static void
vlx_media_discovery_init (VlxMediaDiscovery *self)
{
    (void) self;
}

VlxMediaDiscovery *
vlx_media_discovery_new (VlxPlaylist *playlist)
{
    g_return_val_if_fail (VLX_IS_PLAYLIST (playlist), NULL);

    VlxMediaDiscovery *self = g_object_new (VLX_TYPE_MEDIA_DISCOVERY, NULL);
    self->playlist = g_object_ref (playlist);
    return self;
}

void
vlx_media_discovery_watch (VlxMediaDiscovery *self,
                            const gchar       *directory)
{
    g_return_if_fail (VLX_IS_MEDIA_DISCOVERY (self));
    g_return_if_fail (directory != NULL);

    GFile      *dir = g_file_new_for_path (directory);
    GError     *err = NULL;
    GFileMonitor *monitor = g_file_monitor_directory (
        dir, G_FILE_MONITOR_NONE, NULL, &err);
    g_object_unref (dir);

    if (!monitor) {
        VLX_LOG_ERROR (VLX_LOG_DOMAIN_MEDIA,
                       "Failed to watch %s: %s", directory, err->message);
        g_error_free (err);
        return;
    }

    g_signal_connect (monitor, "changed", G_CALLBACK (on_changed), self);

    WatchEntry *w = g_slice_new0 (WatchEntry);
    w->monitor   = monitor;
    w->directory = g_strdup (directory);
    self->watches = g_list_prepend (self->watches, w);

    scan_directory_initial (self, directory);
    VLX_LOG_INFO (VLX_LOG_DOMAIN_MEDIA, "Watching: %s", directory);
}

void
vlx_media_discovery_unwatch (VlxMediaDiscovery *self,
                              const gchar       *directory)
{
    g_return_if_fail (VLX_IS_MEDIA_DISCOVERY (self));

    for (GList *l = self->watches; l; l = l->next) {
        WatchEntry *w = l->data;
        if (g_strcmp0 (w->directory, directory) == 0) {
            g_file_monitor_cancel (w->monitor);
            g_object_unref (w->monitor);
            g_free (w->directory);
            g_slice_free (WatchEntry, w);
            self->watches = g_list_delete_link (self->watches, l);
            break;
        }
    }
}
