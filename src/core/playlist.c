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
    GHashTable    *reverse_map;  /* Maps un_shuffled_idx -> shuffle_order_idx */
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
rebuild_play_order (VlxPlaylist *self)
{
    g_array_set_size (self->shuffle_map, self->uris->len);
    for (guint i = 0; i < self->uris->len; i++)
        g_array_index (self->shuffle_map, guint, i) = i;

    if (self->shuffle && self->uris->len > 1) {
        /* Fisher-Yates shuffle */
        for (guint i = self->uris->len; i > 1; i--) {
            guint j = g_random_int_range (0, i);
            guint tmp = g_array_index (self->shuffle_map, guint, i - 1);
            g_array_index (self->shuffle_map, guint, i - 1) = g_array_index (self->shuffle_map, guint, j);
            g_array_index (self->shuffle_map, guint, j) = tmp;
        }
        /* Ensure the currently playing item is at the start of the shuffled remainder so we don't repeat it immediately */
        for (guint i = 0; i < self->shuffle_map->len; i++) {
            if (g_array_index (self->shuffle_map, guint, i) == self->current) {
                guint tmp = g_array_index (self->shuffle_map, guint, 0);
                g_array_index (self->shuffle_map, guint, 0) = self->current;
                g_array_index (self->shuffle_map, guint, i) = tmp;
                break;
            }
        }
    }

    g_hash_table_remove_all (self->reverse_map);
    for (guint i = 0; i < self->shuffle_map->len; i++) {
        g_hash_table_insert (self->reverse_map,
                             GUINT_TO_POINTER (g_array_index (self->shuffle_map, guint, i)),
                             GUINT_TO_POINTER (i));
    }
}

static gint
find_order_index (VlxPlaylist *self, guint un_shuffled_idx)
{
    gpointer val;
    if (g_hash_table_lookup_extended (self->reverse_map, GUINT_TO_POINTER (un_shuffled_idx), NULL, &val)) {
        return GPOINTER_TO_UINT (val);
    }
    return -1;
}

static void
vlx_playlist_finalize (GObject *obj)
{
    VlxPlaylist *self = VLX_PLAYLIST (obj);
    g_ptr_array_unref (self->uris);
    g_array_unref (self->shuffle_map);
    g_hash_table_unref (self->reverse_map);
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
    self->reverse_map = g_hash_table_new (NULL, NULL);
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
    guint new_idx = self->uris->len;
    g_ptr_array_add (self->uris, g_strdup (uri));
    
    if (self->shuffle) {
        /* Insert at a random position *after* the current logical position to ensure it gets played */
        gint logical_pos = find_order_index (self, self->current);
        guint insert_pos = self->shuffle_map->len;
        if (logical_pos >= 0 && (guint)logical_pos < self->shuffle_map->len) {
            guint range = self->shuffle_map->len - logical_pos;
            if (range > 0) {
                insert_pos = (guint)logical_pos + 1 + g_random_int_range (0, range);
            }
        }
        insert_pos = MIN (insert_pos, self->shuffle_map->len);
        g_array_insert_val (self->shuffle_map, insert_pos, new_idx);
        
        g_hash_table_remove_all (self->reverse_map);
        for (guint i = 0; i < self->shuffle_map->len; i++) {
            g_hash_table_insert (self->reverse_map,
                                 GUINT_TO_POINTER (g_array_index (self->shuffle_map, guint, i)),
                                 GUINT_TO_POINTER (i));
        }
    } else {
        g_array_append_val (self->shuffle_map, new_idx);
        g_hash_table_insert (self->reverse_map,
                             GUINT_TO_POINTER (new_idx),
                             GUINT_TO_POINTER (self->shuffle_map->len - 1));
    }
    g_signal_emit (self, playlist_signals[SIGNAL_CHANGED], 0);
}

void
vlx_playlist_remove (VlxPlaylist *self, guint index)
{
    g_return_if_fail (VLX_IS_PLAYLIST (self));
    g_return_if_fail (index < self->uris->len);

    g_ptr_array_remove_index (self->uris, index);
    
    /* If the current item was removed, fallback to 0 safely */
    if (self->current == index) {
        self->current = 0;
    } else if (self->current > index) {
        self->current--;
    }
    rebuild_play_order (self);
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
    if (self->current >= self->uris->len) self->current = 0;
    return g_ptr_array_index (self->uris, self->current);
}

const gchar *
vlx_playlist_next (VlxPlaylist *self)
{
    g_return_val_if_fail (VLX_IS_PLAYLIST (self), NULL);
    if (self->uris->len == 0) return NULL;

    if (self->repeat == VLX_REPEAT_ONE)
        return vlx_playlist_get_current (self);

    gint pos = find_order_index (self, self->current);
    if (pos < 0 || (guint)pos + 1 >= self->shuffle_map->len) {
        if (self->repeat == VLX_REPEAT_ALL) {
            rebuild_play_order (self);
            self->current = g_array_index (self->shuffle_map, guint, 0);
        } else {
            return NULL;  /* end of playlist */
        }
    } else {
        self->current = g_array_index (self->shuffle_map, guint, pos + 1);
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

    gint pos = find_order_index (self, self->current);
    if (pos <= 0) {
        if (self->repeat == VLX_REPEAT_ALL) {
            self->current = g_array_index (self->shuffle_map, guint, self->shuffle_map->len - 1);
        } else {
            return NULL;
        }
    } else {
        self->current = g_array_index (self->shuffle_map, guint, pos - 1);
    }
    return vlx_playlist_get_current (self);
}

gboolean
vlx_playlist_set_index (VlxPlaylist *self, guint index)
{
    g_return_val_if_fail (VLX_IS_PLAYLIST (self), FALSE);
    if (index >= self->uris->len) return FALSE;
    self->current = index;
    if (self->shuffle) {
        rebuild_play_order (self);
    }
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
    if (self->shuffle == shuffle) return;
    self->shuffle = shuffle;
    rebuild_play_order (self);
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
