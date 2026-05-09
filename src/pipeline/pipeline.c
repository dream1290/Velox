/* vlx_pipeline.c — GStreamer pipeline lifecycle
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Builds the dynamic pipeline:
 *   uridecodebin3 ──[video]──→ videoconvert → glimagesink
 *                  ──[audio]──→ audioconvert → audioresample → pipewiresink
 *                  ──[text]───→ (subtitle engine)
 */

#include "pipeline/pipeline.h"
#include "pipeline/hwaccel.h"
#include "utils/log.h"
#include <gio/gio.h>

#include <gst/gst.h>
#include <gst/video/video.h>

struct _VlxPipelineManager {
    GObject parent_instance;

    GstElement *pipeline;
    GstElement *uridecodebin;
    GstElement *deinterlace;
    GstElement *video_convert;
    GstElement *videobalance;
    GstElement *subtitle_bin;
    GstElement *video_sink;
    GstElement *audio_convert;
    GstElement *audio_resample;
    GstElement *equalizer;
    GstElement *audio_sink;
    GstElement *audio_volume;

    GSettings  *settings;
    gulong      eq_sig_id;
    gulong      vid_sig_id;

    GstBus     *bus;
    guint       bus_watch_id;

    /* Stream management */
    GstStreamCollection   *collection;
    GList                 *active_stream_ids;

    VlxHwAccelType hwaccel;

    /* Callbacks */
    VlxPipelineStateCb     state_cb;
    VlxPipelineEosCb       eos_cb;
    VlxPipelineErrorCb     error_cb;
    VlxPipelineBufferingCb buf_cb;
    VlxPipelineCollectionCb coll_cb;
    VlxPipelineTocCb        toc_cb;
    gpointer               cb_data;
};

G_DEFINE_TYPE (VlxPipelineManager, vlx_pipeline_manager, G_TYPE_OBJECT)

/* ── Bus message handler ───────────────────────────────────────────────────── */
static gboolean
bus_message_cb (GstBus *bus, GstMessage *msg, gpointer data)
{
    VlxPipelineManager *self = VLX_PIPELINE_MANAGER (data);
    (void) bus;

    switch (GST_MESSAGE_TYPE (msg)) {
    case GST_MESSAGE_STATE_CHANGED: {
        if (GST_MESSAGE_SRC (msg) != GST_OBJECT (self->pipeline))
            break;
        GstState old_st, new_st, pending;
        gst_message_parse_state_changed (msg, &old_st, &new_st, &pending);
        VLX_LOG_DEBUG (VLX_LOG_DOMAIN_PIPELINE,
                       "State: %s → %s",
                       gst_element_state_get_name (old_st),
                       gst_element_state_get_name (new_st));
        if (self->state_cb)
            self->state_cb (self, old_st, new_st, self->cb_data);
        break;
    }
    case GST_MESSAGE_EOS:
        VLX_LOG_INFO (VLX_LOG_DOMAIN_PIPELINE, "EOS received");
        if (self->eos_cb)
            self->eos_cb (self, self->cb_data);
        break;

    case GST_MESSAGE_ERROR: {
        GError *err = NULL;
        gchar  *dbg = NULL;
        gst_message_parse_error (msg, &err, &dbg);
        VLX_LOG_ERROR (VLX_LOG_DOMAIN_PIPELINE,
                       "Pipeline error: %s (debug: %s)",
                       err->message, dbg ? dbg : "none");
        if (self->error_cb)
            self->error_cb (self, err->message, self->cb_data);
        g_error_free (err);
        g_free (dbg);
        break;
    }
    case GST_MESSAGE_BUFFERING: {
        gint percent = 0;
        gst_message_parse_buffering (msg, &percent);
        if (self->buf_cb)
            self->buf_cb (self, percent, self->cb_data);
        break;
    }
    case GST_MESSAGE_STREAM_COLLECTION: {
        GstStreamCollection *collection = NULL;
        gst_message_parse_stream_collection (msg, &collection);
        if (collection) {
            g_set_object (&self->collection, collection);
            if (self->coll_cb)
                self->coll_cb (self, collection, self->cb_data);
            gst_object_unref (collection);
        }
        break;
    }
    case GST_MESSAGE_STREAMS_SELECTED: {
        if (self->active_stream_ids) {
            g_list_free_full (self->active_stream_ids, g_free);
            self->active_stream_ids = NULL;
        }
        guint n = gst_message_streams_selected_get_size (msg);
        for (guint i = 0; i < n; i++) {
            GstStream *stream = gst_message_streams_selected_get_stream (msg, i);
            if (stream) {
                self->active_stream_ids = g_list_append (self->active_stream_ids,
                                                         g_strdup (gst_stream_get_stream_id (stream)));
                gst_object_unref (stream);
            }
        }
        break;
    }
    case GST_MESSAGE_ASYNC_DONE:
        VLX_LOG_DEBUG (VLX_LOG_DOMAIN_PIPELINE, "Async done (seek complete)");
        break;

    case GST_MESSAGE_TOC: {
        GstToc   *toc      = NULL;
        gboolean  updated  = FALSE;
        gst_message_parse_toc (msg, &toc, &updated);
        if (toc) {
            VLX_LOG_INFO (VLX_LOG_DOMAIN_PIPELINE,
                          "TOC received (updated=%s)",
                          updated ? "yes" : "no");
            if (self->toc_cb)
                self->toc_cb (self, toc, self->cb_data);
            gst_toc_unref (toc);
        }
        break;
    }
    default:
        break;
    }

    return G_SOURCE_CONTINUE;
}

/* ── Pad-added callback for uridecodebin3 ──────────────────────────────────── */
static void
on_pad_added (GstElement *src, GstPad *new_pad, gpointer data)
{
    VlxPipelineManager *self = VLX_PIPELINE_MANAGER (data);
    (void) src;

    GstCaps *caps = gst_pad_get_current_caps (new_pad);
    if (!caps)
        caps = gst_pad_query_caps (new_pad, NULL);

    const gchar *name = gst_structure_get_name (gst_caps_get_structure (caps, 0));

    if (g_str_has_prefix (name, "video/")) {
        VLX_LOG_INFO (VLX_LOG_DOMAIN_PIPELINE,
                      "Video pad available: %s", name);
        if (self->deinterlace) {
            GstPad *sink_pad = gst_element_get_static_pad (self->deinterlace, "sink");
            if (sink_pad) {
                if (gst_pad_link (new_pad, sink_pad) != GST_PAD_LINK_OK)
                    VLX_LOG_ERROR (VLX_LOG_DOMAIN_PIPELINE, "Failed to link video pad");
                else
                    VLX_LOG_INFO (VLX_LOG_DOMAIN_PIPELINE, "Video pad linked");
                gst_object_unref (sink_pad);
            }
        }
    } else if (g_str_has_prefix (name, "audio/")) {
        GstPad *sink_pad = gst_element_get_static_pad (self->audio_convert, "sink");
        if (!gst_pad_is_linked (sink_pad)) {
            GstPadLinkReturn ret = gst_pad_link (new_pad, sink_pad);
            if (GST_PAD_LINK_FAILED (ret))
                VLX_LOG_ERROR (VLX_LOG_DOMAIN_PIPELINE,
                               "Failed to link audio pad");
            else
                VLX_LOG_INFO (VLX_LOG_DOMAIN_PIPELINE, "Audio pad linked");
        }
        gst_object_unref (sink_pad);
    } else if (g_str_has_prefix (name, "text/") ||
               g_str_has_prefix (name, "subpicture/")) {
        VLX_LOG_INFO (VLX_LOG_DOMAIN_PIPELINE,
                      "Subtitle pad available: %s", name);
        if (self->subtitle_bin) {
            GstPad *sink_pad = gst_element_request_pad_simple (self->subtitle_bin, "subtitle_sink");
            if (sink_pad) {
                gst_pad_link (new_pad, sink_pad);
                gst_object_unref (sink_pad);
            }
        }
    }

    gst_caps_unref (caps);
}

/* ── Pipeline construction ─────────────────────────────────────────────────── */
static void
on_video_settings_changed (GSettings *settings, const gchar *key, gpointer data)
{
    VlxPipelineManager *self = VLX_PIPELINE_MANAGER (data);
    
    if (!key || g_strcmp0 (key, "deinterlace") == 0) {
        if (self->deinterlace) {
            gboolean deint = g_settings_get_boolean (settings, "deinterlace");
            /* mode: 0=auto, 1=interlaced, 2=disabled */
            g_object_set (self->deinterlace, "mode", deint ? 1 : 2, NULL);
        }
    }
}

static void
on_video_balance_changed (GSettings *settings, const gchar *key, gpointer data)
{
    VlxPipelineManager *self = VLX_PIPELINE_MANAGER (data);
    if (!self->videobalance) return;
    
    /* key is NULL when called manually to set all */
    if (!key || g_strcmp0 (key, "video-brightness") == 0)
        g_object_set (self->videobalance, "brightness", g_settings_get_double (settings, "video-brightness"), NULL);
    if (!key || g_strcmp0 (key, "video-contrast") == 0)
        g_object_set (self->videobalance, "contrast", g_settings_get_double (settings, "video-contrast"), NULL);
    if (!key || g_strcmp0 (key, "video-saturation") == 0)
        g_object_set (self->videobalance, "saturation", g_settings_get_double (settings, "video-saturation"), NULL);
    if (!key || g_strcmp0 (key, "video-hue") == 0)
        g_object_set (self->videobalance, "hue", g_settings_get_double (settings, "video-hue"), NULL);
}

static void
on_eq_gains_changed (GSettings *settings, const gchar *key, gpointer data)
{
    VlxPipelineManager *self = VLX_PIPELINE_MANAGER (data);
    if (!self->equalizer) return;

    GVariant *gains_var = g_settings_get_value (settings, "equalizer-gains");
    gsize n_gains = 0;
    const gdouble *gains = g_variant_get_fixed_array (gains_var, &n_gains, sizeof(gdouble));
    if (n_gains == 10) {
        for (gsize i = 0; i < 10; i++) {
            gchar *prop = g_strdup_printf ("band%zu", i);
            g_object_set (self->equalizer, prop, gains[i], NULL);
            g_free (prop);
        }
    }
    g_variant_unref (gains_var);
}

static void
build_pipeline (VlxPipelineManager *self, const gchar *uri)
{
    /* Tear down old pipeline */
    if (self->pipeline) {
        if (self->settings) {
            if (self->eq_sig_id) {
                g_signal_handler_disconnect (self->settings, self->eq_sig_id);
                self->eq_sig_id = 0;
            }
            if (self->vid_sig_id) {
                g_signal_handler_disconnect (self->settings, self->vid_sig_id);
                self->vid_sig_id = 0;
            }
        }
        gst_element_set_state (self->pipeline, GST_STATE_NULL);
        if (self->bus_watch_id) {
            g_source_remove (self->bus_watch_id);
            self->bus_watch_id = 0;
        }
        if (self->active_stream_ids) {
            g_list_free_full (self->active_stream_ids, g_free);
            self->active_stream_ids = NULL;
        }
        g_clear_object (&self->collection);
        
        gst_object_unref (self->pipeline);
        self->pipeline = NULL;
    }

    self->pipeline = gst_pipeline_new ("velox-pipeline");

    /* Source + decoder */
    self->uridecodebin = gst_element_factory_make ("uridecodebin3", "source");
    g_object_set (self->uridecodebin, "uri", uri, NULL);

    /* Video branch */
    self->deinterlace   = gst_element_factory_make ("deinterlace", "deint");
    self->video_convert = gst_element_factory_make ("videoconvert", "vconv");
    self->videobalance  = gst_element_factory_make ("videobalance", "vbal");
    self->subtitle_bin  = gst_element_factory_make ("subtitleoverlay", "sub_bin");

    /* Use appsink to pull samples into VlxVideoWidget */
    self->video_sink = gst_element_factory_make ("appsink", "vsink");
    if (self->video_sink) {
        GstCaps *caps = gst_caps_from_string ("video/x-raw(memory:GLMemory), format=RGBA; video/x-raw, format=RGBA");
        g_object_set (self->video_sink,
                      "caps", caps,
                      "max-buffers", 2,
                      "drop", TRUE,
                      "sync", TRUE,
                      NULL);
        gst_caps_unref (caps);
    }

    /* Audio branch */
    self->audio_convert   = gst_element_factory_make ("audioconvert", "aconv");
    self->audio_resample  = gst_element_factory_make ("audioresample", "aresample");
    self->equalizer       = gst_element_factory_make ("equalizer-10bands", "eq");
    self->audio_volume    = gst_element_factory_make ("volume", "avol");

    /* Try pipewiresink, fall back to pulsesink or autoaudiosink */
    self->audio_sink = gst_element_factory_make ("pipewiresink", "asink");
    if (!self->audio_sink)
        self->audio_sink = gst_element_factory_make ("pulsesink", "asink");
    if (!self->audio_sink)
        self->audio_sink = gst_element_factory_make ("autoaudiosink", "asink");

    /* Add all elements */
    gst_bin_add_many (GST_BIN (self->pipeline),
                      self->uridecodebin,
                      self->deinterlace, self->video_convert, self->videobalance, self->subtitle_bin, self->video_sink,
                      self->audio_convert, self->audio_resample, self->equalizer,
                      self->audio_volume,  self->audio_sink,
                      NULL);

    /* Link static elements */
    gst_element_link_many (self->deinterlace, self->video_convert, self->videobalance, self->subtitle_bin, self->video_sink, NULL);
    gst_element_link_many (self->audio_convert,
                           self->audio_resample,
                           self->equalizer,
                           self->audio_volume,
                           self->audio_sink,
                           NULL);

    /* Connect dynamic pad signals */
    g_signal_connect (self->uridecodebin, "pad-added",
                      G_CALLBACK (on_pad_added), self);

    /* Setup equalizer from settings */
    if (!self->settings)
        self->settings = g_settings_new ("io.github.velox");
    on_eq_gains_changed (self->settings, "equalizer-gains", self);
    self->eq_sig_id = g_signal_connect (self->settings, "changed::equalizer-gains",
                                        G_CALLBACK (on_eq_gains_changed), self);

    /* Setup videobalance from settings */
    on_video_balance_changed (self->settings, NULL, self);
    self->vid_sig_id = g_signal_connect (self->settings, "changed",
                                         G_CALLBACK (on_video_balance_changed), self);

    /* Setup video settings (deinterlace) */
    on_video_settings_changed (self->settings, NULL, self);
    g_signal_connect (self->settings, "changed::deinterlace",
                      G_CALLBACK (on_video_settings_changed), self);

    /* Watch bus */
    self->bus = gst_pipeline_get_bus (GST_PIPELINE (self->pipeline));
    self->bus_watch_id = gst_bus_add_watch (self->bus, bus_message_cb, self);
    gst_object_unref (self->bus);

    /* Detect hardware acceleration */
    self->hwaccel = vlx_hwaccel_detect ();
    VLX_LOG_INFO (VLX_LOG_DOMAIN_PIPELINE,
                  "HW accel: %s",
                  vlx_hwaccel_type_to_string (self->hwaccel));
}

/* ── GObject boilerplate ───────────────────────────────────────────────────── */
static void
vlx_pipeline_manager_finalize (GObject *obj)
{
    VlxPipelineManager *self = VLX_PIPELINE_MANAGER (obj);

    if (self->pipeline) {
        if (self->settings && self->eq_sig_id) {
            g_signal_handler_disconnect (self->settings, self->eq_sig_id);
            self->eq_sig_id = 0;
        }
        gst_element_set_state (self->pipeline, GST_STATE_NULL);
        if (self->bus_watch_id) {
            g_source_remove (self->bus_watch_id);
            self->bus_watch_id = 0;
        }
        gst_object_unref (self->pipeline);
    }
    if (self->settings) {
        g_object_unref (self->settings);
        self->settings = NULL;
    }
    g_clear_object (&self->collection);
    if (self->active_stream_ids) {
        g_list_free_full (self->active_stream_ids, g_free);
        self->active_stream_ids = NULL;
    }

    G_OBJECT_CLASS (vlx_pipeline_manager_parent_class)->finalize (obj);
}

static void
vlx_pipeline_manager_class_init (VlxPipelineManagerClass *klass)
{
    G_OBJECT_CLASS (klass)->finalize = vlx_pipeline_manager_finalize;
}

static void
vlx_pipeline_manager_init (VlxPipelineManager *self)
{
    self->hwaccel = VLX_HWACCEL_NONE;
}

/* ── Public API ────────────────────────────────────────────────────────────── */
VlxPipelineManager *
vlx_pipeline_manager_new (void)
{
    return g_object_new (VLX_TYPE_PIPELINE_MANAGER, NULL);
}

void
vlx_pipeline_manager_set_callbacks (VlxPipelineManager    *self,
                                    VlxPipelineStateCb     state_cb,
                                    VlxPipelineEosCb       eos_cb,
                                    VlxPipelineErrorCb     error_cb,
                                    VlxPipelineBufferingCb buf_cb,
                                    VlxPipelineCollectionCb coll_cb,
                                    VlxPipelineTocCb        toc_cb,
                                    gpointer               data)
{
    g_return_if_fail (VLX_IS_PIPELINE_MANAGER (self));
    self->state_cb = state_cb;
    self->eos_cb   = eos_cb;
    self->error_cb = error_cb;
    self->buf_cb   = buf_cb;
    self->coll_cb  = coll_cb;
    self->toc_cb   = toc_cb;
    self->cb_data  = data;
}

void
vlx_pipeline_manager_open (VlxPipelineManager *self, const gchar *uri)
{
    g_return_if_fail (VLX_IS_PIPELINE_MANAGER (self));
    build_pipeline (self, uri);
    gst_element_set_state (self->pipeline, GST_STATE_PLAYING);
}

void
vlx_pipeline_manager_set_state (VlxPipelineManager *self, GstState state)
{
    g_return_if_fail (VLX_IS_PIPELINE_MANAGER (self));
    if (self->pipeline)
        gst_element_set_state (self->pipeline, state);
}

void
vlx_pipeline_manager_seek (VlxPipelineManager *self, gint64 position_us)
{
    g_return_if_fail (VLX_IS_PIPELINE_MANAGER (self));
    if (!self->pipeline) return;

    gst_element_seek_simple (self->pipeline,
                             GST_FORMAT_TIME,
                             GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT,
                             position_us * GST_USECOND);
}

gint64
vlx_pipeline_manager_get_position (VlxPipelineManager *self)
{
    g_return_val_if_fail (VLX_IS_PIPELINE_MANAGER (self), 0);
    if (!self->pipeline) return 0;

    gint64 pos = 0;
    if (gst_element_query_position (self->pipeline, GST_FORMAT_TIME, &pos))
        return pos / GST_USECOND;
    return 0;
}

gint64
vlx_pipeline_manager_get_duration (VlxPipelineManager *self)
{
    g_return_val_if_fail (VLX_IS_PIPELINE_MANAGER (self), 0);
    if (!self->pipeline) return 0;

    gint64 dur = 0;
    if (gst_element_query_duration (self->pipeline, GST_FORMAT_TIME, &dur))
        return dur / GST_USECOND;
    return 0;
}

void
vlx_pipeline_manager_set_volume (VlxPipelineManager *self, gdouble volume)
{
    g_return_if_fail (VLX_IS_PIPELINE_MANAGER (self));
    if (self->audio_volume)
        g_object_set (self->audio_volume, "volume", volume, NULL);
}

void
vlx_pipeline_manager_set_muted (VlxPipelineManager *self, gboolean muted)
{
    g_return_if_fail (VLX_IS_PIPELINE_MANAGER (self));
    if (self->audio_volume)
        g_object_set (self->audio_volume, "mute", muted, NULL);
}

void
vlx_pipeline_manager_set_rate (VlxPipelineManager *self, gdouble rate)
{
    g_return_if_fail (VLX_IS_PIPELINE_MANAGER (self));
    if (!self->pipeline) return;

    gint64 pos = 0;
    gst_element_query_position (self->pipeline, GST_FORMAT_TIME, &pos);

    if (rate > 0.0) {
        gst_element_seek (self->pipeline, rate,
                          GST_FORMAT_TIME,
                          GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE,
                          GST_SEEK_TYPE_SET, pos,
                          GST_SEEK_TYPE_SET, -1);
    }
}

void
vlx_pipeline_manager_select_audio (VlxPipelineManager *self, gint index)
{
    g_return_if_fail (VLX_IS_PIPELINE_MANAGER (self));
    if (self->uridecodebin)
        g_object_set (self->uridecodebin, "current-audio", index, NULL);
}

void
vlx_pipeline_manager_select_subtitle (VlxPipelineManager *self, gint index)
{
    /* Not implemented yet */
    (void) self; (void) index;
}

void
vlx_pipeline_manager_set_subtitle_delay (VlxPipelineManager *self, gint64 delay_us)
{
    g_return_if_fail (VLX_IS_PIPELINE_MANAGER (self));
    if (self->subtitle_bin) {
        g_object_set (self->subtitle_bin, "ts-offset", delay_us * 1000, NULL);
    }
}

gint64
vlx_pipeline_manager_get_subtitle_delay (VlxPipelineManager *self)
{
    g_return_val_if_fail (VLX_IS_PIPELINE_MANAGER (self), 0);
    gint64 offset_ns = 0;
    if (self->subtitle_bin) {
        g_object_get (self->subtitle_bin, "ts-offset", &offset_ns, NULL);
    }
    return offset_ns / 1000;
}

void
vlx_pipeline_manager_set_stream (VlxPipelineManager *self, const gchar *new_stream_id)
{
    g_return_if_fail (VLX_IS_PIPELINE_MANAGER (self));
    if (!self->pipeline || !self->collection || !self->active_stream_ids) return;

    GstStreamType new_type = GST_STREAM_TYPE_UNKNOWN;
    for (guint i = 0; i < gst_stream_collection_get_size (self->collection); i++) {
        GstStream *s = gst_stream_collection_get_stream (self->collection, i);
        if (g_strcmp0 (gst_stream_get_stream_id (s), new_stream_id) == 0) {
            new_type = gst_stream_get_stream_type (s);
            break;
        }
    }

    if (new_type == GST_STREAM_TYPE_UNKNOWN) {
        VLX_LOG_ERROR (VLX_LOG_DOMAIN_PIPELINE, "Unknown stream ID: %s", new_stream_id);
        return;
    }

    GList *new_list = NULL;
    for (GList *l = self->active_stream_ids; l != NULL; l = l->next) {
        const gchar *sid = l->data;
        GstStreamType stype = GST_STREAM_TYPE_UNKNOWN;
        
        for (guint i = 0; i < gst_stream_collection_get_size (self->collection); i++) {
            GstStream *s = gst_stream_collection_get_stream (self->collection, i);
            if (g_strcmp0 (gst_stream_get_stream_id (s), sid) == 0) {
                stype = gst_stream_get_stream_type (s);
                break;
            }
        }

        if (stype != new_type) {
            new_list = g_list_append (new_list, g_strdup (sid));
        }
    }

    new_list = g_list_append (new_list, g_strdup (new_stream_id));

    GstEvent *event = gst_event_new_select_streams (new_list);
    if (!gst_element_send_event (self->pipeline, event)) {
        VLX_LOG_ERROR (VLX_LOG_DOMAIN_PIPELINE, "Failed to send SELECT_STREAMS event");
    } else {
        VLX_LOG_INFO (VLX_LOG_DOMAIN_PIPELINE, "Sent SELECT_STREAMS event for stream %s", new_stream_id);
    }
    
    g_list_free_full (new_list, g_free);
}

void
vlx_pipeline_manager_select_streams (VlxPipelineManager *self, GList *stream_ids)
{
    g_return_if_fail (VLX_IS_PIPELINE_MANAGER (self));
    if (!self->pipeline) return;

    GstEvent *event = gst_event_new_select_streams (stream_ids);
    if (!gst_element_send_event (self->pipeline, event)) {
        VLX_LOG_ERROR (VLX_LOG_DOMAIN_PIPELINE, "Failed to send SELECT_STREAMS event");
    } else {
        VLX_LOG_INFO (VLX_LOG_DOMAIN_PIPELINE, "Sent SELECT_STREAMS event");
    }
}

GstElement *
vlx_pipeline_manager_get_video_sink (VlxPipelineManager *self)
{
    g_return_val_if_fail (VLX_IS_PIPELINE_MANAGER (self), NULL);
    return self->video_sink;
}

/* ── Video balance live setters ──────────────────────────────────────────── */
void
vlx_pipeline_manager_set_brightness (VlxPipelineManager *self, gdouble val)
{
    g_return_if_fail (VLX_IS_PIPELINE_MANAGER (self));
    if (self->videobalance)
        g_object_set (self->videobalance, "brightness", CLAMP (val, -1.0, 1.0), NULL);
}

void
vlx_pipeline_manager_set_contrast (VlxPipelineManager *self, gdouble val)
{
    g_return_if_fail (VLX_IS_PIPELINE_MANAGER (self));
    if (self->videobalance)
        g_object_set (self->videobalance, "contrast", CLAMP (val, 0.0, 2.0), NULL);
}

void
vlx_pipeline_manager_set_saturation (VlxPipelineManager *self, gdouble val)
{
    g_return_if_fail (VLX_IS_PIPELINE_MANAGER (self));
    if (self->videobalance)
        g_object_set (self->videobalance, "saturation", CLAMP (val, 0.0, 2.0), NULL);
}

void
vlx_pipeline_manager_set_hue (VlxPipelineManager *self, gdouble val)
{
    g_return_if_fail (VLX_IS_PIPELINE_MANAGER (self));
    if (self->videobalance)
        g_object_set (self->videobalance, "hue", CLAMP (val, -1.0, 1.0), NULL);
}

/* ── External subtitle file ──────────────────────────────────────────────── */
void
vlx_pipeline_manager_load_subtitle_file (VlxPipelineManager *self,
                                         const gchar        *path)
{
    g_return_if_fail (VLX_IS_PIPELINE_MANAGER (self));
    g_return_if_fail (path != NULL);
    if (!self->uridecodebin) return;

    gchar *sub_uri = gst_filename_to_uri (path, NULL);
    if (!sub_uri) return;

    VLX_LOG_INFO (VLX_LOG_DOMAIN_PIPELINE,
                  "Loading external subtitle: %s", sub_uri);

    g_object_set (self->uridecodebin, "suburi", sub_uri, NULL);
    g_free (sub_uri);
}
