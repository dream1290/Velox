/* vlx_playlist.h — Ring-buffer playlist with shuffle/repeat
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

#define VLX_TYPE_PLAYLIST (vlx_playlist_get_type ())
G_DECLARE_FINAL_TYPE (VlxPlaylist, vlx_playlist, VLX, PLAYLIST, GObject)

typedef enum {
    VLX_REPEAT_NONE,
    VLX_REPEAT_ONE,
    VLX_REPEAT_ALL,
} VlxRepeatMode;

VlxPlaylist *vlx_playlist_new        (void);

void         vlx_playlist_append     (VlxPlaylist *self, const gchar *uri);
void         vlx_playlist_remove     (VlxPlaylist *self, guint index);
void         vlx_playlist_clear      (VlxPlaylist *self);

const gchar *vlx_playlist_get_current (VlxPlaylist *self);
const gchar *vlx_playlist_next        (VlxPlaylist *self);
const gchar *vlx_playlist_previous    (VlxPlaylist *self);
gboolean     vlx_playlist_set_index   (VlxPlaylist *self, guint index);

guint        vlx_playlist_get_length  (VlxPlaylist *self);
guint        vlx_playlist_get_index   (VlxPlaylist *self);
const gchar *vlx_playlist_get_uri_at  (VlxPlaylist *self, guint index);

void         vlx_playlist_set_shuffle (VlxPlaylist *self, gboolean shuffle);
gboolean     vlx_playlist_get_shuffle (VlxPlaylist *self);
void         vlx_playlist_set_repeat  (VlxPlaylist *self, VlxRepeatMode mode);
VlxRepeatMode vlx_playlist_get_repeat (VlxPlaylist *self);

G_END_DECLS
