/* vlx_event_bus.h — GSignal-based publish/subscribe event mesh
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * VlxEventBus is the sole coupling point between the UI (L1) and the
 * media engine (L3).  All state changes, position ticks, and errors
 * flow through typed GSignals on this singleton GObject.
 *
 * Design rationale:
 *   - UI code connects to VlxEventBus signals; it never imports GStreamer.
 *   - The pipeline module emits through the bus; it never imports GTK.
 *   - This gives us a clean compile-time firewall between layers.
 */

#pragma once

#include <glib-object.h>
#include <gst/gst.h>

G_BEGIN_DECLS

/* ── Type macros ───────────────────────────────────────────────────────────── */
#define VLX_TYPE_EVENT_BUS (vlx_event_bus_get_type ())
G_DECLARE_FINAL_TYPE (VlxEventBus, vlx_event_bus, VLX, EVENT_BUS, GObject)

/* ── Player states (mirrors VlxPlayerState) ────────────────────────────────── */
typedef enum {
    VLX_STATE_IDLE,
    VLX_STATE_LOADING,
    VLX_STATE_READY,
    VLX_STATE_PLAYING,
    VLX_STATE_PAUSED,
    VLX_STATE_STOPPED,
    VLX_STATE_ERROR,
} VlxPlayerState;

GType vlx_player_state_get_type (void);
#define VLX_TYPE_PLAYER_STATE (vlx_player_state_get_type ())

/* ── Singleton accessor ────────────────────────────────────────────────────── */

/**
 * vlx_event_bus_get_default:
 *
 * Returns: (transfer none): The process-wide event bus instance.
 */
VlxEventBus *vlx_event_bus_get_default (void);

/* ── Emission helpers (called from L2/L3) ──────────────────────────────────── */

void vlx_event_bus_emit_state_changed    (VlxEventBus   *bus,
                                          VlxPlayerState  new_state);

void vlx_event_bus_emit_position_updated (VlxEventBus *bus,
                                          gint64       position_us,
                                          gint64       duration_us);

void vlx_event_bus_emit_error            (VlxEventBus *bus,
                                          const gchar *domain,
                                          const gchar *message);

void vlx_event_bus_emit_media_info       (VlxEventBus *bus,
                                          const gchar *title,
                                          const gchar *uri,
                                          gint64       duration_us);

void vlx_event_bus_emit_buffering        (VlxEventBus *bus,
                                          gint         percent);
void vlx_event_bus_emit_stream_collection(VlxEventBus *bus,
                                          GstStreamCollection *collection);

void vlx_event_bus_emit_eos              (VlxEventBus *bus);

void vlx_event_bus_emit_volume_changed   (VlxEventBus *bus,
                                          gdouble      volume);

void vlx_event_bus_emit_seek_done        (VlxEventBus *bus);

/* A-B loop: a_us/b_us are -1 when not set */
void vlx_event_bus_emit_ab_loop          (VlxEventBus *bus,
                                          gint64       a_us,
                                          gint64       b_us,
                                          gboolean     active);

/* Chapter TOC */
void vlx_event_bus_emit_chapters_updated (VlxEventBus *bus,
                                          const GArray *chapters_us);  /* gint64[] */

G_END_DECLS
