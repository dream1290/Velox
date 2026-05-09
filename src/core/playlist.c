/* vlx_playlist.c — Ring-buffer playlist with shuffle/repeat
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/playlist.h"
#include "utils/log.h"

struct _VlxPlaylist {
    GObject parent_instance;

    GPtrArray     *uris;         /* gchar* entries, owned */
    GArray        *shuffle_map;  /* guint indices for shuffle order */
    guint          current;
    gboolean       shuffle;
    VlxRepeatMode  repeat;
};

G_DEFINE_TYPE (VlxPlaylist, vlx_playlist, G_TYPE_OBJECT)

enum {
    SIGNAL_CHANGED,
    N_SIGNALS,
};
static guint playlist_signals[N_SIGNALS];

static void
rebuild_shuffle_map (VlxPlaylist *self)
{
    g_array_set_size (self->shuffle_map, self->uris->len);
    for (guint i = 0; i < self->uris->len; i++)
        g_array_index (self->shuffle_map, guint, i) = i;

    /* Fisher-Yates shuffle */
    for (guint i = self->uris->len; i > 1; i--) {
        guint j = g_random_int_range (0, i);
        guint tmp = g_array_index (self->shuffle_map, guint, i - 1);
        g_array_index (self->shuffle_map, guint, i - 1) =
            g_array_index (self->shuffle_map, guint, j);
        g_array_index (self->shuffle_map, guint, j) = tmp;
    }
}

static guint
resolve_index (VlxPlaylist *self, guint logical)
{
    if (self->shuffle && self->shuffle_map->len > 0)
        return g_array_index (self->shuffle_map, guint, logical);
    return logical;
}

static void
vlx_playlist_finalize (GObject *obj)
{
    VlxPlaylist *self = VLX_PLAYLIST (obj);
    g_ptr_array_unref (self->uris);
    g_array_unref (self->shuffle_map);
    G_OBJECT_CLASS (vlx_playlist_parent_class)->finalize (obj);
}

static void
vlx_playlist_class_init (VlxPlaylistClass *klass)
{
    G_OBJECT_CLASS (klass)->finalize = vlx_playlist_finalize;

    playlist_signals[SIGNAL_CHANGED] =
        g_signal_new ("changed",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_LAST,
                      0, NULL, NULL, NULL,
                      G_TYPE_NONE, 0);
}

static void
vlx_playlist_init (VlxPlaylist *self)
{
    self->uris        = g_ptr_array_new_with_free_func (g_free);
    self->shuffle_map = g_array_new (FALSE, FALSE, sizeof (guint));
    self->current     = 0;
    self->shuffle     = FALSE;
    self->repeat      = VLX_REPEAT_NONE;
}

VlxPlaylist *
vlx_playlist_new (void)
{
    return g_object_new (VLX_TYPE_PLAYLIST, NULL);
}

void
vlx_playlist_append (VlxPlaylist *self, const gchar *uri)
{
    g_return_if_fail (VLX_IS_PLAYLIST (self));
    g_ptr_array_add (self->uris, g_strdup (uri));
    if (self->shuffle)
        rebuild_shuffle_map (self);
    g_signal_emit (self, playlist_signals[SIGNAL_CHANGED], 0);
}

void
vlx_playlist_remove (VlxPlaylist *self, guint index)
{
    g_return_if_fail (VLX_IS_PLAYLIST (self));
    g_return_if_fail (index < self->uris->len);

    g_ptr_array_remove_index (self->uris, index);
    if (self->current >= self->uris->len && self->uris->len > 0)
        self->current = self->uris->len - 1;
    if (self->shuffle)
        rebuild_shuffle_map (self);
    g_signal_emit (self, playlist_signals[SIGNAL_CHANGED], 0);
}

void
vlx_playlist_clear (VlxPlaylist *self)
{
    g_return_if_fail (VLX_IS_PLAYLIST (self));
    g_ptr_array_set_size (self->uris, 0);
    g_array_set_size (self->shuffle_map, 0);
    self->current = 0;
    g_signal_emit (self, playlist_signals[SIGNAL_CHANGED], 0);
}

const gchar *
vlx_playlist_get_current (VlxPlaylist *self)
{
    g_return_val_if_fail (VLX_IS_PLAYLIST (self), NULL);
    if (self->uris->len == 0) return NULL;
    guint idx = resolve_index (self, self->current);
    return g_ptr_array_index (self->uris, idx);
}

const gchar *
vlx_playlist_next (VlxPlaylist *self)
{
    g_return_val_if_fail (VLX_IS_PLAYLIST (self), NULL);
    if (self->uris->len == 0) return NULL;

    if (self->repeat == VLX_REPEAT_ONE)
        return vlx_playlist_get_current (self);

    self->current++;
    if (self->current >= self->uris->len) {
        if (self->repeat == VLX_REPEAT_ALL) {
            self->current = 0;
            if (self->shuffle)
                rebuild_shuffle_map (self);
        } else {
            self->current = self->uris->len - 1;
            return NULL;  /* end of playlist */
        }
    }
    return vlx_playlist_get_current (self);
}

const gchar *
vlx_playlist_previous (VlxPlaylist *self)
{
    g_return_val_if_fail (VLX_IS_PLAYLIST (self), NULL);
    if (self->uris->len == 0) return NULL;

    if (self->repeat == VLX_REPEAT_ONE)
        return vlx_playlist_get_current (self);

    if (self->current == 0) {
        if (self->repeat == VLX_REPEAT_ALL)
            self->current = self->uris->len - 1;
        else
            return NULL;
    } else {
        self->current--;
    }
    return vlx_playlist_get_current (self);
}

gboolean
vlx_playlist_set_index (VlxPlaylist *self, guint index)
{
    g_return_val_if_fail (VLX_IS_PLAYLIST (self), FALSE);
    if (index >= self->uris->len) return FALSE;
    self->current = index;
    return TRUE;
}

guint vlx_playlist_get_length (VlxPlaylist *self)
{
    g_return_val_if_fail (VLX_IS_PLAYLIST (self), 0);
    return self->uris->len;
}

guint vlx_playlist_get_index (VlxPlaylist *self)
{
    g_return_val_if_fail (VLX_IS_PLAYLIST (self), 0);
    return self->current;
}

const gchar *
vlx_playlist_get_uri_at (VlxPlaylist *self, guint index)
{
    g_return_val_if_fail (VLX_IS_PLAYLIST (self), NULL);
    if (index >= self->uris->len) return NULL;
    return g_ptr_array_index (self->uris, index);
}

void vlx_playlist_set_shuffle (VlxPlaylist *self, gboolean shuffle)
{
    g_return_if_fail (VLX_IS_PLAYLIST (self));
    self->shuffle = shuffle;
    if (shuffle)
        rebuild_shuffle_map (self);
}

gboolean vlx_playlist_get_shuffle (VlxPlaylist *self)
{
    g_return_val_if_fail (VLX_IS_PLAYLIST (self), FALSE);
    return self->shuffle;
}

void vlx_playlist_set_repeat (VlxPlaylist *self, VlxRepeatMode mode)
{
    g_return_if_fail (VLX_IS_PLAYLIST (self));
    self->repeat = mode;
}

VlxRepeatMode vlx_playlist_get_repeat (VlxPlaylist *self)
{
    g_return_val_if_fail (VLX_IS_PLAYLIST (self), VLX_REPEAT_NONE);
    return self->repeat;
}
