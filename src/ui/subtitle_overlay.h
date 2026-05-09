/* vlx_subtitle_overlay.h — Pango text overlay for subtitles
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define VLX_TYPE_SUBTITLE_OVERLAY (vlx_subtitle_overlay_get_type ())
G_DECLARE_FINAL_TYPE (VlxSubtitleOverlay, vlx_subtitle_overlay,
                      VLX, SUBTITLE_OVERLAY, GtkWidget)

VlxSubtitleOverlay *vlx_subtitle_overlay_new (void);
void vlx_subtitle_overlay_set_text (VlxSubtitleOverlay *self,
                                    const gchar *text);

G_END_DECLS
