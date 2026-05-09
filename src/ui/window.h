/* vlx_window.h — Main application window (AdwApplicationWindow)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <adwaita.h>
#include "core/player.h"
#include "core/playlist.h"

G_BEGIN_DECLS

#define VLX_TYPE_WINDOW (vlx_window_get_type ())
G_DECLARE_FINAL_TYPE (VlxWindow, vlx_window, VLX, WINDOW, AdwApplicationWindow)

VlxWindow *vlx_window_new   (AdwApplication *app);
void       vlx_window_open  (VlxWindow *self, const gchar *uri);
VlxPlayer *vlx_window_get_player (VlxWindow *self);
VlxPlaylist *vlx_window_get_playlist (VlxWindow *self);

G_END_DECLS
