/* vlx_player.c — Central player state machine
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/player.h"
#include "core/event_bus.h"
#include "pipeline/pipeline.h"
#include "utils/log.h"

#include <gst/gst.h>

/* ── Private data ──────────────────────────────────────────────────────────── */
struct _VlxPlayer {
    GObject parent_instance;

    VlxPipelineManager *pipeline;
    VlxEventBus        *bus;

    VlxPlayerState  state;
    gchar          *current_uri;
    gdouble         volume;
    gboolean        muted;
    gdouble         rate;
    gint64          duration_us;
    gint64          last_toggle_time_us;

    /* A-B loop */
    gint64          ab_a_us;
    gint64          ab_b_us;
    gboolean        ab_active;

    /* Chapter list (sorted) */
    GArray         *chapters_us;   /* gint64 timestamps in µs */

    guint           tick_source_id;
};

G_DEFINE_TYPE (VlxPlayer, vlx_player, G_TYPE_OBJECT)

/* ── Properties ────────────────────────────────────────────────────────────── */
enum {
    PROP_0,
    PROP_STATE,
    PROP_VOLUME,
    PROP_MUTED,
    PROP_RATE,
    PROP_URI,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ── Position tick (16ms ≈ 60 Hz) ──────────────────────────────────────────── */
static gboolean
position_tick_cb (gpointer data)
{
    VlxPlayer *self = VLX_PLAYER (data);

    if (self->state != VLX_STATE_PLAYING)
        return G_SOURCE_CONTINUE;

    gint64 pos = vlx_pipeline_manager_get_position (self->pipeline);
    gint64 dur = vlx_pipeline_manager_get_duration (self->pipeline);

    if (dur > 0)
        self->duration_us = dur;

    vlx_event_bus_emit_position_updated (self->bus, pos, self->duration_us);

    /* ── A-B loop enforcement ── */
    if (self->ab_active && self->ab_b_us > self->ab_a_us && pos >= self->ab_b_us) {
        vlx_pipeline_manager_seek (self->pipeline, self->ab_a_us);
    }

    return G_SOURCE_CONTINUE;
}

static void
start_tick (VlxPlayer *self)
{
    if (self->tick_source_id == 0)
        self->tick_source_id = g_timeout_add (16, position_tick_cb, self);
}

static void
stop_tick (VlxPlayer *self)
{
    if (self->tick_source_id != 0) {
        g_source_remove (self->tick_source_id);
        self->tick_source_id = 0;
    }
}

/* ── State transition ──────────────────────────────────────────────────────── */
static void
set_state (VlxPlayer *self, VlxPlayerState new_state)
{
    if (self->state == new_state)
        return;

    self->state = new_state;
    vlx_event_bus_emit_state_changed (self->bus, new_state);
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_STATE]);
}

/* ── Pipeline callbacks ────────────────────────────────────────────────────── */
static void
on_pipeline_state_changed (VlxPipelineManager *pm,
                           GstState            old_state,
                           GstState            new_state,
                           gpointer            data)
{
    VlxPlayer *self = VLX_PLAYER (data);
    (void) pm;
    (void) old_state;

    switch (new_state) {
    case GST_STATE_PLAYING:
        set_state (self, VLX_STATE_PLAYING);
        start_tick (self);
        break;
    case GST_STATE_PAUSED:
        set_state (self, VLX_STATE_PAUSED);
        break;
    case GST_STATE_READY:
        set_state (self, VLX_STATE_READY);
        break;
    case GST_STATE_NULL:
        set_state (self, VLX_STATE_STOPPED);
        stop_tick (self);
        break;
    default:
        break;
    }
}

static void
on_pipeline_eos (VlxPipelineManager *pm, gpointer data)
{
    VlxPlayer *self = VLX_PLAYER (data);
    (void) pm;

    stop_tick (self);
    vlx_event_bus_emit_eos (self->bus);
    set_state (self, VLX_STATE_STOPPED);
}

static void
on_pipeline_error (VlxPipelineManager *pm,
                   const gchar        *message,
                   gpointer            data)
{
    VlxPlayer *self = VLX_PLAYER (data);
    (void) pm;

    stop_tick (self);
    vlx_event_bus_emit_error (self->bus, "Pipeline", message);
    set_state (self, VLX_STATE_ERROR);
}

static void
on_pipeline_buffering (VlxPipelineManager *pm,
                       gint                percent,
                       gpointer            data)
{
    VlxPlayer *self = VLX_PLAYER (data);
    (void) pm;

    vlx_event_bus_emit_buffering (self->bus, percent);

    if (percent < 100) {
        if (self->state == VLX_STATE_PLAYING)
            vlx_pipeline_manager_set_state (self->pipeline, GST_STATE_PAUSED);
        set_state (self, VLX_STATE_LOADING);
    } else {
        vlx_pipeline_manager_set_state (self->pipeline, GST_STATE_PLAYING);
    }
}

static void
on_pipeline_collection (VlxPipelineManager  *pm,
                        GstStreamCollection *collection,
                        gpointer             data)
{
    VlxPlayer *self = VLX_PLAYER (data);
    (void) pm;

    vlx_event_bus_emit_stream_collection (self->bus, collection);
}

static void
on_pipeline_toc (VlxPipelineManager *pm,
                 GstToc             *toc,
                 gpointer            data)
{
    VlxPlayer *self = VLX_PLAYER (data);
    (void) pm;

    /* Walk the TOC, collect chapter start times */
    GArray *chapters = g_array_new (FALSE, FALSE, sizeof (gint64));

    GList *entries = gst_toc_get_entries (toc);
    for (GList *l = entries; l != NULL; l = l->next) {
        GstTocEntry *entry = (GstTocEntry *) l->data;
        if (gst_toc_entry_get_entry_type (entry) == GST_TOC_ENTRY_TYPE_CHAPTER) {
            gint64 start = 0, stop = 0;
            gst_toc_entry_get_start_stop_times (entry, &start, &stop);
            g_array_append_val (chapters, start);
        }
    }

    VLX_LOG_INFO (VLX_LOG_DOMAIN_CORE,
                  "TOC: %u chapter(s) found", chapters->len);

    /* Store in player for seek_chapter() */
    vlx_player_set_chapters (self, chapters);

    /* Broadcast on event bus for seek bar tick marks */
    vlx_event_bus_emit_chapters_updated (self->bus, chapters);

    g_array_unref (chapters);
}

/* ── GObject overrides ─────────────────────────────────────────────────────── */
static void
vlx_player_get_property (GObject    *obj,
                         guint       prop_id,
                         GValue     *value,
                         GParamSpec *pspec)
{
    VlxPlayer *self = VLX_PLAYER (obj);
    switch (prop_id) {
    case PROP_STATE:  g_value_set_enum   (value, self->state);  break;
    case PROP_VOLUME: g_value_set_double (value, self->volume); break;
    case PROP_MUTED:  g_value_set_boolean(value, self->muted);  break;
    case PROP_RATE:   g_value_set_double (value, self->rate);   break;
    case PROP_URI:    g_value_set_string (value, self->current_uri); break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID (obj, prop_id, pspec);
    }
}

static void
vlx_player_set_property (GObject      *obj,
                         guint         prop_id,
                         const GValue *value,
                         GParamSpec   *pspec)
{
    VlxPlayer *self = VLX_PLAYER (obj);
    switch (prop_id) {
    case PROP_VOLUME:
        vlx_player_set_volume (self, g_value_get_double (value));
        break;
    case PROP_MUTED:
        vlx_player_set_muted (self, g_value_get_boolean (value));
        break;
    case PROP_RATE:
        vlx_player_set_rate (self, g_value_get_double (value));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (obj, prop_id, pspec);
    }
}

static void
vlx_player_finalize (GObject *obj)
{
    VlxPlayer *self = VLX_PLAYER (obj);

    stop_tick (self);
    g_clear_object (&self->pipeline);
    g_free (self->current_uri);
    if (self->chapters_us)
        g_array_unref (self->chapters_us);
    G_OBJECT_CLASS (vlx_player_parent_class)->finalize (obj);
}

static void
vlx_player_class_init (VlxPlayerClass *klass)
{
    GObjectClass *obj_class = G_OBJECT_CLASS (klass);
    obj_class->get_property = vlx_player_get_property;
    obj_class->set_property = vlx_player_set_property;
    obj_class->finalize     = vlx_player_finalize;

    props[PROP_STATE] = g_param_spec_enum (
        "state", NULL, NULL,
        VLX_TYPE_PLAYER_STATE, VLX_STATE_IDLE,
        G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    props[PROP_VOLUME] = g_param_spec_double (
        "volume", NULL, NULL,
        0.0, 1.0, 1.0,
        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_MUTED] = g_param_spec_boolean (
        "muted", NULL, NULL,
        FALSE,
        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_RATE] = g_param_spec_double (
        "rate", NULL, NULL,
        0.25, 4.0, 1.0,
        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_URI] = g_param_spec_string (
        "uri", NULL, NULL, NULL,
        G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (obj_class, N_PROPS, props);
}

static void
vlx_player_init (VlxPlayer *self)
{
    self->bus        = vlx_event_bus_get_default ();
    self->state      = VLX_STATE_IDLE;
    self->volume     = 1.0;
    self->rate       = 1.0;
    self->muted      = FALSE;
    self->ab_a_us    = -1;
    self->ab_b_us    = -1;
    self->ab_active  = FALSE;
    self->chapters_us = g_array_new (FALSE, FALSE, sizeof (gint64));
}

/* ── Public API ────────────────────────────────────────────────────────────── */
VlxPlayer *
vlx_player_new (void)
{
    return g_object_new (VLX_TYPE_PLAYER, NULL);
}

void
vlx_player_open (VlxPlayer *self, const gchar *uri)
{
    g_return_if_fail (VLX_IS_PLAYER (self));
    g_return_if_fail (uri != NULL);

    vlx_player_stop (self);

    g_free (self->current_uri);
    self->current_uri = g_strdup (uri);
    self->duration_us = 0;

    set_state (self, VLX_STATE_LOADING);

    /* Create pipeline if needed */
    if (!self->pipeline)
        self->pipeline = vlx_pipeline_manager_new ();

    vlx_pipeline_manager_set_callbacks (self->pipeline,
                                        on_pipeline_state_changed,
                                        on_pipeline_eos,
                                        on_pipeline_error,
                                        on_pipeline_buffering,
                                        on_pipeline_collection,
                                        on_pipeline_toc,
                                        self);

    vlx_pipeline_manager_open (self->pipeline, uri);
    vlx_pipeline_manager_set_volume (self->pipeline, self->volume);

    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_URI]);

    VLX_LOG_INFO (VLX_LOG_DOMAIN_CORE, "Opening: %s", uri);
}

void vlx_player_play (VlxPlayer *self)
{
    g_return_if_fail (VLX_IS_PLAYER (self));
    if (self->pipeline)
        vlx_pipeline_manager_set_state (self->pipeline, GST_STATE_PLAYING);
}

void vlx_player_pause (VlxPlayer *self)
{
    g_return_if_fail (VLX_IS_PLAYER (self));
    if (self->pipeline)
        vlx_pipeline_manager_set_state (self->pipeline, GST_STATE_PAUSED);
}

void vlx_player_stop (VlxPlayer *self)
{
    g_return_if_fail (VLX_IS_PLAYER (self));
    stop_tick (self);
    if (self->pipeline)
        vlx_pipeline_manager_set_state (self->pipeline, GST_STATE_NULL);
    set_state (self, VLX_STATE_STOPPED);
}

void vlx_player_toggle (VlxPlayer *self)
{
    g_return_if_fail (VLX_IS_PLAYER (self));
    
    gint64 now = g_get_monotonic_time ();
    if (now - self->last_toggle_time_us < 200000) {
        return; /* Throttle rapid toggles */
    }
    self->last_toggle_time_us = now;

    if (self->state == VLX_STATE_PLAYING)
        vlx_player_pause (self);
    else
        vlx_player_play (self);
}

void vlx_player_seek (VlxPlayer *self, gint64 position_us)
{
    g_return_if_fail (VLX_IS_PLAYER (self));
    if (self->pipeline) {
        vlx_pipeline_manager_seek (self->pipeline, position_us);
    }
}

void vlx_player_seek_relative (VlxPlayer *self, gint64 offset_us)
{
    g_return_if_fail (VLX_IS_PLAYER (self));
    gint64 pos = vlx_player_get_position (self);
    gint64 target = CLAMP (pos + offset_us, 0, self->duration_us);
    vlx_player_seek (self, target);
}

void vlx_player_set_volume (VlxPlayer *self, gdouble volume)
{
    g_return_if_fail (VLX_IS_PLAYER (self));
    self->volume = CLAMP (volume, 0.0, 1.0);
    if (self->pipeline)
        vlx_pipeline_manager_set_volume (self->pipeline, self->volume);
    vlx_event_bus_emit_volume_changed (self->bus, self->volume);
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_VOLUME]);
}

gdouble vlx_player_get_volume (VlxPlayer *self)
{
    g_return_val_if_fail (VLX_IS_PLAYER (self), 1.0);
    return self->volume;
}

void vlx_player_set_muted (VlxPlayer *self, gboolean muted)
{
    g_return_if_fail (VLX_IS_PLAYER (self));
    self->muted = muted;
    if (self->pipeline)
        vlx_pipeline_manager_set_muted (self->pipeline, muted);
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_MUTED]);
}

gboolean vlx_player_get_muted (VlxPlayer *self)
{
    g_return_val_if_fail (VLX_IS_PLAYER (self), FALSE);
    return self->muted;
}

VlxPlayerState vlx_player_get_state (VlxPlayer *self)
{
    g_return_val_if_fail (VLX_IS_PLAYER (self), VLX_STATE_IDLE);
    return self->state;
}

gint64 vlx_player_get_position (VlxPlayer *self)
{
    g_return_val_if_fail (VLX_IS_PLAYER (self), 0);
    if (!self->pipeline) return 0;
    return vlx_pipeline_manager_get_position (self->pipeline);
}

gint64 vlx_player_get_duration (VlxPlayer *self)
{
    g_return_val_if_fail (VLX_IS_PLAYER (self), 0);
    return self->duration_us;
}

const gchar *vlx_player_get_uri (VlxPlayer *self)
{
    g_return_val_if_fail (VLX_IS_PLAYER (self), NULL);
    return self->current_uri;
}

void vlx_player_set_subtitle_track (VlxPlayer *self, gint idx)
{
    g_return_if_fail (VLX_IS_PLAYER (self));
    if (self->pipeline)
        vlx_pipeline_manager_select_subtitle (self->pipeline, idx);
}

void vlx_player_set_subtitle_delay (VlxPlayer *self, gint64 delay_us)
{
    g_return_if_fail (VLX_IS_PLAYER (self));
    if (self->pipeline)
        vlx_pipeline_manager_set_subtitle_delay (self->pipeline, delay_us);
}

gint64 vlx_player_get_subtitle_delay (VlxPlayer *self)
{
    g_return_val_if_fail (VLX_IS_PLAYER (self), 0);
    if (self->pipeline)
        return vlx_pipeline_manager_get_subtitle_delay (self->pipeline);
    return 0;
}

void vlx_player_set_audio_track (VlxPlayer *self, gint idx)
{
    g_return_if_fail (VLX_IS_PLAYER (self));
    if (self->pipeline)
        vlx_pipeline_manager_select_audio (self->pipeline, idx);
}

void
vlx_player_select_stream (VlxPlayer *self, const gchar *stream_id)
{
    g_return_if_fail (VLX_IS_PLAYER (self));
    if (self->pipeline)
        vlx_pipeline_manager_set_stream (self->pipeline, stream_id);
}

void vlx_player_set_rate (VlxPlayer *self, gdouble rate)
{
    g_return_if_fail (VLX_IS_PLAYER (self));
    self->rate = CLAMP (rate, 0.25, 4.0);
    if (self->pipeline)
        vlx_pipeline_manager_set_rate (self->pipeline, self->rate);
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_RATE]);
}

gdouble vlx_player_get_rate (VlxPlayer *self)
{
    g_return_val_if_fail (VLX_IS_PLAYER (self), 1.0);
    return self->rate;
}

GstElement *vlx_player_get_video_sink (VlxPlayer *self)
{
    g_return_val_if_fail (VLX_IS_PLAYER (self), NULL);
    if (!self->pipeline) return NULL;
    return vlx_pipeline_manager_get_video_sink (self->pipeline);
}

/* ── A-B Loop ──────────────────────────────────────────────────────────────── */
void
vlx_player_set_ab_a (VlxPlayer *self)
{
    g_return_if_fail (VLX_IS_PLAYER (self));
    self->ab_a_us   = vlx_player_get_position (self);
    self->ab_b_us   = -1;
    self->ab_active = FALSE;
    VLX_LOG_INFO (VLX_LOG_DOMAIN_CORE, "A-B: A set at %lld µs", (long long)self->ab_a_us);
    vlx_event_bus_emit_ab_loop (self->bus, self->ab_a_us, self->ab_b_us, FALSE);
}

void
vlx_player_set_ab_b (VlxPlayer *self)
{
    g_return_if_fail (VLX_IS_PLAYER (self));
    if (self->ab_a_us < 0) return;   /* A not set yet */
    gint64 b = vlx_player_get_position (self);
    if (b <= self->ab_a_us) return;  /* B must be after A */
    self->ab_b_us   = b;
    self->ab_active = TRUE;
    VLX_LOG_INFO (VLX_LOG_DOMAIN_CORE, "A-B: B set at %lld µs, loop active", (long long)self->ab_b_us);
    vlx_event_bus_emit_ab_loop (self->bus, self->ab_a_us, self->ab_b_us, TRUE);
}

void
vlx_player_clear_ab (VlxPlayer *self)
{
    g_return_if_fail (VLX_IS_PLAYER (self));
    self->ab_a_us   = -1;
    self->ab_b_us   = -1;
    self->ab_active = FALSE;
    VLX_LOG_INFO (VLX_LOG_DOMAIN_CORE, "A-B: loop cleared");
    vlx_event_bus_emit_ab_loop (self->bus, -1, -1, FALSE);
}

gboolean
vlx_player_get_ab_state (VlxPlayer *self, gint64 *a_out, gint64 *b_out)
{
    g_return_val_if_fail (VLX_IS_PLAYER (self), FALSE);
    if (a_out) *a_out = self->ab_a_us;
    if (b_out) *b_out = self->ab_b_us;
    return self->ab_active;
}

/* ── Chapter Navigation ────────────────────────────────────────────────────── */
void
vlx_player_set_chapters (VlxPlayer *self, const GArray *chapters_us)
{
    g_return_if_fail (VLX_IS_PLAYER (self));
    g_array_set_size (self->chapters_us, 0);
    if (chapters_us) {
        for (guint i = 0; i < chapters_us->len; i++) {
            gint64 t = g_array_index (chapters_us, gint64, i);
            g_array_append_val (self->chapters_us, t);
        }
    }
}

void
vlx_player_seek_chapter (VlxPlayer *self, gint delta)
{
    g_return_if_fail (VLX_IS_PLAYER (self));
    if (!self->chapters_us || self->chapters_us->len == 0) {
        /* No chapters: jump ±90 seconds */
        vlx_player_seek_relative (self, (gint64)delta * 90 * G_USEC_PER_SEC);
        return;
    }

    gint64 pos = vlx_player_get_position (self);
    gint cur = -1;

    /* Find which chapter we're currently in */
    for (guint i = 0; i < self->chapters_us->len; i++) {
        if (pos >= g_array_index (self->chapters_us, gint64, i))
            cur = (gint)i;
    }

    gint target = CLAMP (cur + delta, 0, (gint)self->chapters_us->len - 1);
    gint64 dest = g_array_index (self->chapters_us, gint64, target);
    vlx_player_seek (self, dest);
}

/* ── Video balance (pass-through to pipeline) ────────────────────────────── */
void vlx_player_set_brightness (VlxPlayer *self, gdouble val)
{
    g_return_if_fail (VLX_IS_PLAYER (self));
    if (self->pipeline)
        vlx_pipeline_manager_set_brightness (self->pipeline, val);
}

void vlx_player_set_contrast (VlxPlayer *self, gdouble val)
{
    g_return_if_fail (VLX_IS_PLAYER (self));
    if (self->pipeline)
        vlx_pipeline_manager_set_contrast (self->pipeline, val);
}

void vlx_player_set_saturation (VlxPlayer *self, gdouble val)
{
    g_return_if_fail (VLX_IS_PLAYER (self));
    if (self->pipeline)
        vlx_pipeline_manager_set_saturation (self->pipeline, val);
}

void vlx_player_set_hue (VlxPlayer *self, gdouble val)
{
    g_return_if_fail (VLX_IS_PLAYER (self));
    if (self->pipeline)
        vlx_pipeline_manager_set_hue (self->pipeline, val);
}

/* ── External subtitle file ──────────────────────────────────────────────── */
void vlx_player_load_subtitle_file (VlxPlayer *self, const gchar *path)
{
    g_return_if_fail (VLX_IS_PLAYER (self));
    if (self->pipeline)
        vlx_pipeline_manager_load_subtitle_file (self->pipeline, path);
}
