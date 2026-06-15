/* vlx_player.h — Central player state machine (L2)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <glib-object.h>
#include <gst/gst.h>
#include "core/event_bus.h"

G_BEGIN_DECLS

#define VLX_TYPE_PLAYER (vlx_player_get_type ())
G_DECLARE_FINAL_TYPE (VlxPlayer, vlx_player, VLX, PLAYER, GObject)

VlxPlayer     *vlx_player_new           (void);

void           vlx_player_open          (VlxPlayer *self, const gchar *uri);
void           vlx_player_play          (VlxPlayer *self);
void           vlx_player_pause         (VlxPlayer *self);
void           vlx_player_stop          (VlxPlayer *self);
void           vlx_player_toggle        (VlxPlayer *self);
void           vlx_player_seek          (VlxPlayer *self, gint64 position_us);
void           vlx_player_seek_relative (VlxPlayer *self, gint64 offset_us);

void           vlx_player_set_volume    (VlxPlayer *self, gdouble volume);
gdouble        vlx_player_get_volume    (VlxPlayer *self);
void           vlx_player_set_muted     (VlxPlayer *self, gboolean muted);
gboolean       vlx_player_get_muted     (VlxPlayer *self);

VlxPlayerState vlx_player_get_state     (VlxPlayer *self);
gint64         vlx_player_get_position  (VlxPlayer *self);
gint64         vlx_player_get_duration  (VlxPlayer *self);
const gchar   *vlx_player_get_uri       (VlxPlayer *self);

void           vlx_player_select_stream      (VlxPlayer *self, const gchar *stream_id);

void           vlx_player_set_subtitle_delay (VlxPlayer *self, gint64 delay_us);
gint64         vlx_player_get_subtitle_delay (VlxPlayer *self);
void           vlx_player_set_rate           (VlxPlayer *self, gdouble rate);
gdouble        vlx_player_get_rate           (VlxPlayer *self);

GstElement    *vlx_player_get_video_sink     (VlxPlayer *self);

/* A-B loop */
void           vlx_player_set_ab_a           (VlxPlayer *self);
void           vlx_player_set_ab_b           (VlxPlayer *self);
void           vlx_player_clear_ab           (VlxPlayer *self);
gboolean       vlx_player_get_ab_state       (VlxPlayer *self,
                                              gint64    *a_out,
                                              gint64    *b_out);

/* Chapter navigation */
void           vlx_player_set_chapters       (VlxPlayer    *self,
                                              const GArray *chapters_us);
void           vlx_player_seek_chapter       (VlxPlayer *self, gint delta);

/* Video balance */
void           vlx_player_set_brightness     (VlxPlayer *self, gdouble val);

/* External subtitle file */
void           vlx_player_load_subtitle_file (VlxPlayer *self,
                                              const gchar *path);

G_END_DECLS
