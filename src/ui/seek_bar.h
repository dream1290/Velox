/* vlx_seek_bar.h — Custom Cairo-drawn seek bar
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define VLX_TYPE_SEEK_BAR (vlx_seek_bar_get_type ())
G_DECLARE_FINAL_TYPE (VlxSeekBar, vlx_seek_bar, VLX, SEEK_BAR, GtkWidget)

VlxSeekBar *vlx_seek_bar_new (void);

void vlx_seek_bar_set_position (VlxSeekBar *self, gdouble fraction);
void vlx_seek_bar_set_buffered (VlxSeekBar *self, gdouble fraction);
void vlx_seek_bar_set_duration (VlxSeekBar *self, gint64 duration_us);
void vlx_seek_bar_set_uri      (VlxSeekBar *self, const gchar *uri);

/* Chapters / markers */
void vlx_seek_bar_add_chapter  (VlxSeekBar *self, gdouble fraction,
                                const gchar *label);
void vlx_seek_bar_clear_chapters (VlxSeekBar *self);

/* A-B loop markers (fraction = -1.0 to clear) */
void vlx_seek_bar_set_ab_markers (VlxSeekBar *self,
                                  gdouble     a_frac,
                                  gdouble     b_frac,
                                  gboolean    active);

G_END_DECLS
