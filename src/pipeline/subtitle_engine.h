/* vlx_subtitle_engine.h — Subtitle parsing + rendering
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

#define VLX_TYPE_SUBTITLE_ENGINE (vlx_subtitle_engine_get_type ())
G_DECLARE_FINAL_TYPE (VlxSubtitleEngine, vlx_subtitle_engine,
                      VLX, SUBTITLE_ENGINE, GObject)

typedef struct {
    gint64  start_us;
    gint64  end_us;
    gchar  *text;     /* Pango markup */
} VlxSubtitleSpan;

VlxSubtitleEngine *vlx_subtitle_engine_new (void);

gboolean vlx_subtitle_engine_load_file (VlxSubtitleEngine *self,
                                        const gchar       *path);

const VlxSubtitleSpan *vlx_subtitle_engine_get_span_at (
    VlxSubtitleEngine *self,
    gint64             position_us);

void vlx_subtitle_engine_clear (VlxSubtitleEngine *self);

G_END_DECLS
