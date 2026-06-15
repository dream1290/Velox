/* vlx_event_bus.c — GSignal pub/sub mesh implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/event_bus.h"
#include "utils/log.h"

/* ── GEnum registration for VlxPlayerState ─────────────────────────────────── */
GType
vlx_player_state_get_type (void)
{
    static gsize type_id = 0;
    if (g_once_init_enter (&type_id)) {
        static const GEnumValue values[] = {
            { VLX_STATE_IDLE,    "VLX_STATE_IDLE",    "idle"    },
            { VLX_STATE_LOADING, "VLX_STATE_LOADING", "loading" },
            { VLX_STATE_READY,   "VLX_STATE_READY",   "ready"   },
            { VLX_STATE_PLAYING, "VLX_STATE_PLAYING", "playing" },
            { VLX_STATE_PAUSED,  "VLX_STATE_PAUSED",  "paused"  },
            { VLX_STATE_STOPPED, "VLX_STATE_STOPPED", "stopped" },
            { VLX_STATE_ERROR,   "VLX_STATE_ERROR",   "error"   },
            { 0, NULL, NULL },
        };
        GType id = g_enum_register_static ("VlxPlayerState", values);
        g_once_init_leave (&type_id, id);
    }
    return type_id;
}

/* ── Signal IDs ────────────────────────────────────────────────────────────── */
enum {
    SIGNAL_STATE_CHANGED,
    SIGNAL_POSITION_UPDATED,
    SIGNAL_ERROR,
    SIGNAL_MEDIA_INFO,
    SIGNAL_BUFFERING,
    SIGNAL_STREAM_COLLECTION,
    SIGNAL_EOS,
    SIGNAL_VOLUME_CHANGED,
    SIGNAL_SEEK_DONE,
    SIGNAL_AB_LOOP,
    SIGNAL_CHAPTERS_UPDATED,
    N_SIGNALS,
};

static guint signals[N_SIGNALS];

/* ── GObject boilerplate ───────────────────────────────────────────────────── */
struct _VlxEventBus {
    GObject parent_instance;
};

G_DEFINE_TYPE (VlxEventBus, vlx_event_bus, G_TYPE_OBJECT)

static void
vlx_event_bus_class_init (VlxEventBusClass *klass)
{
    /**
     * VlxEventBus::state-changed:
     * @bus: the event bus
     * @new_state: the new VlxPlayerState
     */
    signals[SIGNAL_STATE_CHANGED] =
        g_signal_new ("state-changed",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_LAST,
                      0, NULL, NULL, NULL,
                      G_TYPE_NONE, 1,
                      VLX_TYPE_PLAYER_STATE);

    /**
     * VlxEventBus::position-updated:
     * @bus: the event bus
     * @position_us: current position in microseconds
     * @duration_us: total duration in microseconds
     */
    signals[SIGNAL_POSITION_UPDATED] =
        g_signal_new ("position-updated",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_LAST,
                      0, NULL, NULL, NULL,
                      G_TYPE_NONE, 2,
                      G_TYPE_INT64, G_TYPE_INT64);

    /**
     * VlxEventBus::error:
     * @bus: the event bus
     * @domain: error domain string
     * @message: human-readable error message
     */
    signals[SIGNAL_ERROR] =
        g_signal_new ("error",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_LAST,
                      0, NULL, NULL, NULL,
                      G_TYPE_NONE, 2,
                      G_TYPE_STRING, G_TYPE_STRING);

    /**
     * VlxEventBus::media-info:
     * @bus: the event bus
     * @title: media title
     * @uri: source URI
     * @duration_us: duration in microseconds
     */
    signals[SIGNAL_MEDIA_INFO] =
        g_signal_new ("media-info",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_LAST,
                      0, NULL, NULL, NULL,
                      G_TYPE_NONE, 3,
                      G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT64);

    /**
     * VlxEventBus::buffering:
     * @bus: the event bus
     * @percent: buffering progress (0–100)
     */
    signals[SIGNAL_BUFFERING] =
        g_signal_new ("buffering",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_LAST,
                      0, NULL, NULL, NULL,
                      G_TYPE_NONE, 1,
                      G_TYPE_INT);

    /**
     * VlxEventBus::stream-collection:
     * @bus: the event bus
     * @collection: the GstStreamCollection
     */
    signals[SIGNAL_STREAM_COLLECTION] =
        g_signal_new ("stream-collection",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_LAST,
                      0, NULL, NULL, NULL,
                      G_TYPE_NONE, 1,
                      GST_TYPE_STREAM_COLLECTION);

    /**
     * VlxEventBus::eos:
     * @bus: the event bus
     *
     * Emitted when the pipeline reaches end-of-stream.
     */
    signals[SIGNAL_EOS] =
        g_signal_new ("eos",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_LAST,
                      0, NULL, NULL, NULL,
                      G_TYPE_NONE, 0);

    /**
     * VlxEventBus::volume-changed:
     * @bus: the event bus
     * @volume: new volume (0.0–1.0)
     */
    signals[SIGNAL_VOLUME_CHANGED] =
        g_signal_new ("volume-changed",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_LAST,
                      0, NULL, NULL, NULL,
                      G_TYPE_NONE, 1,
                      G_TYPE_DOUBLE);

    /**
     * VlxEventBus::seek-done:
     * @bus: the event bus
     *
     * Emitted when an asynchronous seek completes.
     */
    signals[SIGNAL_SEEK_DONE] =
        g_signal_new ("seek-done",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_LAST,
                      0, NULL, NULL, NULL,
                      G_TYPE_NONE, 0);

    /* ab-loop: a_us (int64), b_us (int64), active (boolean) */
    signals[SIGNAL_AB_LOOP] =
        g_signal_new ("ab-loop",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_LAST,
                      0, NULL, NULL, NULL,
                      G_TYPE_NONE, 3,
                      G_TYPE_INT64, G_TYPE_INT64, G_TYPE_BOOLEAN);

    /* chapters-updated: GVariant of type ax (array of int64) */
    signals[SIGNAL_CHAPTERS_UPDATED] =
        g_signal_new ("chapters-updated",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_LAST,
                      0, NULL, NULL, NULL,
                      G_TYPE_NONE, 1,
                      G_TYPE_VARIANT);
}

static void
vlx_event_bus_init (VlxEventBus *self)
{
    (void) self;
}

/* ── Singleton ─────────────────────────────────────────────────────────────── */
VlxEventBus *
vlx_event_bus_get_default (void)
{
    static VlxEventBus *instance = NULL;

    if (g_once_init_enter_pointer (&instance)) {
        VlxEventBus *bus = g_object_new (VLX_TYPE_EVENT_BUS, NULL);
        g_once_init_leave_pointer (&instance, bus);
    }

    return instance;
}

/* ── Emission helpers ──────────────────────────────────────────────────────── */
void
vlx_event_bus_emit_state_changed (VlxEventBus   *bus,
                                  VlxPlayerState  new_state)
{
    g_return_if_fail (VLX_IS_EVENT_BUS (bus));
    VLX_LOG_DEBUG (VLX_LOG_DOMAIN_CORE, "State → %d", new_state);
    g_signal_emit (bus, signals[SIGNAL_STATE_CHANGED], 0, new_state);
}

void
vlx_event_bus_emit_position_updated (VlxEventBus *bus,
                                     gint64       position_us,
                                     gint64       duration_us)
{
    g_return_if_fail (VLX_IS_EVENT_BUS (bus));
    g_signal_emit (bus, signals[SIGNAL_POSITION_UPDATED], 0,
                   position_us, duration_us);
}

void
vlx_event_bus_emit_error (VlxEventBus *bus,
                          const gchar *domain,
                          const gchar *message)
{
    g_return_if_fail (VLX_IS_EVENT_BUS (bus));
    VLX_LOG_ERROR (VLX_LOG_DOMAIN_CORE, "[%s] %s", domain, message);
    g_signal_emit (bus, signals[SIGNAL_ERROR], 0, domain, message);
}

void
vlx_event_bus_emit_media_info (VlxEventBus *bus,
                               const gchar *title,
                               const gchar *uri,
                               gint64       duration_us)
{
    g_return_if_fail (VLX_IS_EVENT_BUS (bus));
    VLX_LOG_INFO (VLX_LOG_DOMAIN_CORE,
                  "Media: \"%s\" (%s) duration=%" G_GINT64_FORMAT "µs",
                  title, uri, duration_us);
    g_signal_emit (bus, signals[SIGNAL_MEDIA_INFO], 0,
                   title, uri, duration_us);
}

void
vlx_event_bus_emit_buffering (VlxEventBus *bus,
                              gint         percent)
{
    g_return_if_fail (VLX_IS_EVENT_BUS (bus));
    g_signal_emit (bus, signals[SIGNAL_BUFFERING], 0, percent);
}

void
vlx_event_bus_emit_stream_collection (VlxEventBus *bus, GstStreamCollection *collection)
{
    g_return_if_fail (VLX_IS_EVENT_BUS (bus));
    g_return_if_fail (GST_IS_STREAM_COLLECTION (collection));
    g_signal_emit (bus, signals[SIGNAL_STREAM_COLLECTION], 0, collection);
}

void
vlx_event_bus_emit_eos (VlxEventBus *bus)
{
    g_return_if_fail (VLX_IS_EVENT_BUS (bus));
    VLX_LOG_INFO (VLX_LOG_DOMAIN_CORE, "End of stream");
    g_signal_emit (bus, signals[SIGNAL_EOS], 0);
}

void
vlx_event_bus_emit_volume_changed (VlxEventBus *bus,
                                   gdouble      volume)
{
    g_return_if_fail (VLX_IS_EVENT_BUS (bus));
    g_signal_emit (bus, signals[SIGNAL_VOLUME_CHANGED], 0, volume);
}

void
vlx_event_bus_emit_seek_done (VlxEventBus *bus)
{
    g_return_if_fail (VLX_IS_EVENT_BUS (bus));
    VLX_LOG_DEBUG (VLX_LOG_DOMAIN_CORE, "Seek completed");
    g_signal_emit (bus, signals[SIGNAL_SEEK_DONE], 0);
}

void
vlx_event_bus_emit_ab_loop (VlxEventBus *bus,
                             gint64       a_us,
                             gint64       b_us,
                             gboolean     active)
{
    g_return_if_fail (VLX_IS_EVENT_BUS (bus));
    g_signal_emit (bus, signals[SIGNAL_AB_LOOP], 0, a_us, b_us, active);
}

void
vlx_event_bus_emit_chapters_updated (VlxEventBus *bus, const GArray *chapters_us)
{
    g_return_if_fail (VLX_IS_EVENT_BUS (bus));
    GVariantBuilder builder;
    g_variant_builder_init (&builder, G_VARIANT_TYPE ("ax"));
    if (chapters_us) {
        for (guint i = 0; i < chapters_us->len; i++) {
            gint64 t = g_array_index (chapters_us, gint64, i);
            g_variant_builder_add (&builder, "x", t);
        }
    }
    GVariant *v = g_variant_builder_end (&builder);
    g_variant_ref_sink (v);
    g_signal_emit (bus, signals[SIGNAL_CHAPTERS_UPDATED], 0, v);
    g_variant_unref (v);
}
