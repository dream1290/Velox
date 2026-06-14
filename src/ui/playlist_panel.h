/* vlx_playlist_panel.h — Sidebar playlist with thumbnails
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <adwaita.h>
#include "core/playlist.h"

G_BEGIN_DECLS

#define VLX_TYPE_PLAYLIST_PANEL (vlx_playlist_panel_get_type ())
G_DECLARE_FINAL_TYPE (VlxPlaylistPanel, vlx_playlist_panel,
                      VLX, PLAYLIST_PANEL, GtkWidget)

VlxPlaylistPanel *vlx_playlist_panel_new             (VlxPlaylist      *playlist);
void              vlx_playlist_panel_set_playing      (VlxPlaylistPanel *self);

G_END_DECLS
