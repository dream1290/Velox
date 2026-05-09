/* vlx_history.h — SQLite-backed watch history (L4)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Schema (single table):
 *   history(uri TEXT PK, title TEXT, last_pos_us INTEGER,
 *           duration_us INTEGER, play_count INTEGER, last_watched TEXT)
 *
 * Consumers:
 *   • vlx_window: on media-info → record; on stop → update position
 *   • vlx_window: on open → seek to resume position (if > 5%)
 *   • playlist_panel: "Continue Watching" section
 */

#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

#define VLX_TYPE_HISTORY (vlx_history_get_type ())
G_DECLARE_FINAL_TYPE (VlxHistory, vlx_history, VLX, HISTORY, GObject)

typedef struct {
    gchar  *uri;
    gchar  *title;
    gint64  last_pos_us;
    gint64  duration_us;
    gint    play_count;
    gchar  *last_watched;  /* ISO-8601 timestamp */
} VlxHistoryEntry;

VlxHistory       *vlx_history_get_default        (void);

/* Record / update an entry.  Creates it if new, increments play_count. */
void              vlx_history_record             (VlxHistory  *self,
                                                  const gchar *uri,
                                                  const gchar *title,
                                                  gint64       duration_us);

/* Update the resume position (called on stop/close). */
void              vlx_history_update_position    (VlxHistory  *self,
                                                  const gchar *uri,
                                                  gint64       position_us);

/* Get resume position; returns 0 if unknown or < 5% watched. */
gint64            vlx_history_get_resume_pos     (VlxHistory  *self,
                                                  const gchar *uri);

/* Returns a GPtrArray of VlxHistoryEntry*, most-recent first. Caller frees. */
GPtrArray        *vlx_history_list               (VlxHistory  *self,
                                                  guint        max_entries);

void              vlx_history_entry_free         (VlxHistoryEntry *entry);

/* Remove a single entry or clear all history. */
void              vlx_history_remove             (VlxHistory  *self,
                                                  const gchar *uri);
void              vlx_history_clear_all          (VlxHistory  *self);

G_END_DECLS
