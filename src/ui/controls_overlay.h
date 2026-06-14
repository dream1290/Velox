/* vlx_controls_overlay.h — Auto-hiding playback controls
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <adwaita.h>
#include "core/player.h"

G_BEGIN_DECLS

#define VLX_TYPE_CONTROLS_OVERLAY (vlx_controls_overlay_get_type ())
G_DECLARE_FINAL_TYPE (VlxControlsOverlay, vlx_controls_overlay,
                      VLX, CONTROLS_OVERLAY, GtkWidget)

VlxControlsOverlay *vlx_controls_overlay_new          (VlxPlayer *player);
void                vlx_controls_overlay_set_buffering (VlxControlsOverlay *self, gdouble fraction);
void                vlx_controls_overlay_set_fullscreen_state (VlxControlsOverlay *self, gboolean is_fullscreen);
void                vlx_controls_overlay_show_briefly  (VlxControlsOverlay *self);
void                vlx_controls_overlay_seek_ripple   (VlxControlsOverlay *self,
                                                        gboolean            forward);

G_END_DECLS
