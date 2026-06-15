/* vlx_pipeline.c — GStreamer pipeline lifecycle
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Builds a robust pipeline using playbin3 for maximum reliability
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
    GstElement *playbin;
    GstElement *video_sink;
    GstElement *audio_sink;

    GSettings *settings;
    gulong eq_sig_id;
    gulong vid_sig_id;
    gulong deint_sig_id;

    GstBus *bus;
    guint bus_watch_id;

    GstStreamCollection *collection;
    GList *active_stream_ids;

    VlxHwAccelType hwaccel;

    VlxPipelineStateCb state_cb;
    VlxPipelineEosCb eos_cb;
    VlxPipelineErrorCb error_cb;
    VlxPipelineBufferingCb buf_cb;
    VlxPipelineCollectionCb coll_cb;
    VlxPipelineTocCb toc_cb;
    gpointer cb_data;

    GMutex seek_mutex;
    GCond seek_cond;
    gint64 seek_target_us;
    gboolean seek_exit;
    gboolean seek_in_progress;
    GThread *seek_thread;
};

G_DEFINE_TYPE(VlxPipelineManager, vlx_pipeline_manager, G_TYPE_OBJECT)

static gpointer seek_thread_func(gpointer data) {
    VlxPipelineManager *self = VLX_PIPELINE_MANAGER(data);
    g_mutex_lock(&self->seek_mutex);
    while (!self->seek_exit) {
        if (self->seek_target_us >= 0) {
            gint64 target = self->seek_target_us;
            self->seek_target_us = -1;
            GstElement *pipe = self->pipeline ? gst_object_ref(self->pipeline) : NULL;
            self->seek_in_progress = TRUE;
            g_mutex_unlock(&self->seek_mutex);

            if (pipe) {
                gst_element_seek_simple(pipe,
                                        GST_FORMAT_TIME,
                                        GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT | GST_SEEK_FLAG_SKIP,
                                        target * GST_USECOND);
                gst_object_unref(pipe);
            }
            g_mutex_lock(&self->seek_mutex);
            self->seek_in_progress = FALSE;
            g_cond_signal(&self->seek_cond);
        } else {
            g_cond_wait(&self->seek_cond, &self->seek_mutex);
        }
    }
    g_mutex_unlock(&self->seek_mutex);
    return NULL;
}

static gboolean bus_message_cb(GstBus *bus, GstMessage *msg, gpointer data) {
    VlxPipelineManager *self = VLX_PIPELINE_MANAGER(data);
    (void)bus;

    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_STATE_CHANGED: {
            if (GST_MESSAGE_SRC(msg) != GST_OBJECT(self->pipeline))
                break;
            GstState old_st, new_st, pending;
            gst_message_parse_state_changed(msg, &old_st, &new_st, &pending);
            VLX_LOG_DEBUG(VLX_LOG_DOMAIN_PIPELINE, "State: %s → %s", gst_element_state_get_name(old_st), gst_element_state_get_name(new_st));
            if (self->state_cb)
                self->state_cb(self, old_st, new_st, self->cb_data);
            break;
        }
        case GST_MESSAGE_EOS:
            VLX_LOG_INFO(VLX_LOG_DOMAIN_PIPELINE, "EOS received");
            if (self->eos_cb)
                self->eos_cb(self, self->cb_data);
            break;
        case GST_MESSAGE_ERROR: {
            GError *err = NULL;
            gchar *dbg = NULL;
            gst_message_parse_error(msg, &err, &dbg);
            VLX_LOG_ERROR(VLX_LOG_DOMAIN_PIPELINE, "Pipeline error: %s (debug: %s)", err->message, dbg ? dbg : "none");
            if (self->error_cb)
                self->error_cb(self, err->message, self->cb_data);
            g_error_free(err);
            g_free(dbg);
            break;
        }
        case GST_MESSAGE_BUFFERING: {
            gint percent = 0;
            gst_message_parse_buffering(msg, &percent);
            if (self->buf_cb)
                self->buf_cb(self, percent, self->cb_data);
            break;
        }
        case GST_MESSAGE_STREAM_COLLECTION: {
            GstStreamCollection *collection = NULL;
            gst_message_parse_stream_collection(msg, &collection);
            if (collection) {
                g_set_object(&self->collection, collection);
                if (self->coll_cb)
                    self->coll_cb(self, collection, self->cb_data);
                gst_object_unref(collection);
            }
            break;
        }
        case GST_MESSAGE_STREAMS_SELECTED: {
            if (self->active_stream_ids) {
                g_list_free_full(self->active_stream_ids, g_free);
                self->active_stream_ids = NULL;
            }
            guint n = gst_message_streams_selected_get_size(msg);
            for (guint i = 0; i < n; i++) {
                GstStream *stream = gst_message_streams_selected_get_stream(msg, i);
                if (stream) {
                    self->active_stream_ids = g_list_append(self->active_stream_ids, g_strdup(gst_stream_get_stream_id(stream)));
                    gst_object_unref(stream);
                }
            }
            break;
        }
        case GST_MESSAGE_ASYNC_DONE:
            VLX_LOG_DEBUG(VLX_LOG_DOMAIN_PIPELINE, "Async done (seek complete)");
            break;
        case GST_MESSAGE_TOC: {
            GstToc *toc = NULL;
            gboolean updated = FALSE;
            gst_message_parse_toc(msg, &toc, &updated);
            if (toc) {
                VLX_LOG_INFO(VLX_LOG_DOMAIN_PIPELINE, "TOC received (updated=%s)", updated ? "yes" : "no");
                if (self->toc_cb)
                    self->toc_cb(self, toc, self->cb_data);
                gst_toc_unref(toc);
            }
            break;
        }
        default:
            break;
    }
    return G_SOURCE_CONTINUE;
}

static void on_video_balance_changed(GSettings *settings, const gchar *key, gpointer data) {
    // VlxPipelineManager *self = VLX_PIPELINE_MANAGER(data);
    // if (!self->pipeline) return;
    // ... disable video-filter due to playbin3 bugs
}

static void build_pipeline(VlxPipelineManager *self, const gchar *uri) {
    g_mutex_lock(&self->seek_mutex);
    self->seek_target_us = -1;
    while (self->seek_in_progress) {
        g_cond_wait(&self->seek_cond, &self->seek_mutex);
    }
    g_mutex_unlock(&self->seek_mutex);

    if (self->pipeline) {
        if (self->settings) {
            if (self->eq_sig_id) {
                g_signal_handler_disconnect(self->settings, self->eq_sig_id);
                self->eq_sig_id = 0;
            }
            if (self->vid_sig_id) {
                g_signal_handler_disconnect(self->settings, self->vid_sig_id);
                self->vid_sig_id = 0;
            }
            if (self->deint_sig_id) {
                g_signal_handler_disconnect(self->settings, self->deint_sig_id);
                self->deint_sig_id = 0;
            }
        }
        gst_element_set_state(self->pipeline, GST_STATE_NULL);
        if (self->bus_watch_id) {
            g_source_remove(self->bus_watch_id);
            self->bus_watch_id = 0;
        }
        if (self->active_stream_ids) {
            g_list_free_full(self->active_stream_ids, g_free);
            self->active_stream_ids = NULL;
        }
        g_clear_object(&self->collection);

        self->playbin = NULL;
        self->video_sink = NULL;
        self->audio_sink = NULL;
        gst_object_unref(self->pipeline);
        self->pipeline = NULL;
    }

    self->pipeline = gst_pipeline_new("velox-pipeline");
    self->playbin = gst_element_factory_make("playbin3", "playbin");
    self->video_sink = gst_element_factory_make("appsink", "vsink");

    if (self->video_sink) {
        GstCaps *caps = gst_caps_from_string("video/x-raw, format=RGBA, pixel-aspect-ratio=1/1");
        g_object_set(self->video_sink, "caps", caps, "max-buffers", 2, "drop", TRUE, "sync", TRUE, NULL);
        gst_caps_unref(caps);
        g_object_set(self->playbin, "video-sink", self->video_sink, NULL);
    }

    self->audio_sink = gst_element_factory_make("pipewiresink", "asink");
    if (!self->audio_sink)
        self->audio_sink = gst_element_factory_make("pulsesink", "asink");
    if (!self->audio_sink)
        self->audio_sink = gst_element_factory_make("autoaudiosink", "asink");
    if (self->audio_sink)
        g_object_set(self->playbin, "audio-sink", self->audio_sink, NULL);

    g_object_set(self->playbin, "uri", uri, NULL);
    gst_bin_add(GST_BIN(self->pipeline), self->playbin);

    if (!self->settings)
        self->settings = g_settings_new("io.github.velox");

    on_video_balance_changed(self->settings, NULL, self);
    self->vid_sig_id = g_signal_connect(self->settings, "changed", G_CALLBACK(on_video_balance_changed), self);

    self->bus = gst_pipeline_get_bus(GST_PIPELINE(self->pipeline));
    self->bus_watch_id = gst_bus_add_watch(self->bus, bus_message_cb, self);
    gst_object_unref(self->bus);
}

static void vlx_pipeline_manager_finalize(GObject *obj) {
    VlxPipelineManager *self = VLX_PIPELINE_MANAGER(obj);

    g_mutex_lock(&self->seek_mutex);
    self->seek_exit = TRUE;
    g_cond_signal(&self->seek_cond);
    g_mutex_unlock(&self->seek_mutex);

    if (self->seek_thread) {
        g_thread_join(self->seek_thread);
        self->seek_thread = NULL;
    }
    g_mutex_clear(&self->seek_mutex);
    g_cond_clear(&self->seek_cond);

    if (self->pipeline) {
        if (self->settings) {
            if (self->eq_sig_id) {
                g_signal_handler_disconnect(self->settings, self->eq_sig_id);
                self->eq_sig_id = 0;
            }
            if (self->vid_sig_id) {
                g_signal_handler_disconnect(self->settings, self->vid_sig_id);
                self->vid_sig_id = 0;
            }
            if (self->deint_sig_id) {
                g_signal_handler_disconnect(self->settings, self->deint_sig_id);
                self->deint_sig_id = 0;
            }
        }
        gst_element_set_state(self->pipeline, GST_STATE_NULL);
        if (self->bus_watch_id) {
            g_source_remove(self->bus_watch_id);
            self->bus_watch_id = 0;
        }
        gst_object_unref(self->pipeline);
    }
    if (self->settings) {
        g_object_unref(self->settings);
        self->settings = NULL;
    }
    g_clear_object(&self->collection);
    if (self->active_stream_ids) {
        g_list_free_full(self->active_stream_ids, g_free);
        self->active_stream_ids = NULL;
    }

    G_OBJECT_CLASS(vlx_pipeline_manager_parent_class)->finalize(obj);
}

static void vlx_pipeline_manager_class_init(VlxPipelineManagerClass *klass) {
    GObjectClass *obj_class = G_OBJECT_CLASS(klass);
    obj_class->finalize = vlx_pipeline_manager_finalize;
}

static void vlx_pipeline_manager_init(VlxPipelineManager *self) {
    self->hwaccel = VLX_HWACCEL_NONE;
    g_mutex_init(&self->seek_mutex);
    g_cond_init(&self->seek_cond);
    self->seek_target_us = -1;
    self->seek_exit = FALSE;
    self->seek_in_progress = FALSE;
    self->seek_thread = g_thread_new("vlx-seek", seek_thread_func, self);
}

VlxPipelineManager *vlx_pipeline_manager_new(void) {
    return g_object_new(VLX_TYPE_PIPELINE_MANAGER, NULL);
}

void vlx_pipeline_manager_set_callbacks(VlxPipelineManager *self, VlxPipelineStateCb state_cb, VlxPipelineEosCb eos_cb, VlxPipelineErrorCb error_cb, VlxPipelineBufferingCb buf_cb, VlxPipelineCollectionCb coll_cb, VlxPipelineTocCb toc_cb, gpointer data) {
    g_return_if_fail(VLX_IS_PIPELINE_MANAGER(self));
    self->state_cb = state_cb;
    self->eos_cb = eos_cb;
    self->error_cb = error_cb;
    self->buf_cb = buf_cb;
    self->coll_cb = coll_cb;
    self->toc_cb = toc_cb;
    self->cb_data = data;
}

void vlx_pipeline_manager_open(VlxPipelineManager *self, const gchar *uri) {
    g_return_if_fail(VLX_IS_PIPELINE_MANAGER(self));
    build_pipeline(self, uri);
    gst_element_set_state(self->pipeline, GST_STATE_PLAYING);
}

void vlx_pipeline_manager_set_state(VlxPipelineManager *self, GstState state) {
    g_return_if_fail(VLX_IS_PIPELINE_MANAGER(self));
    if (self->pipeline)
        gst_element_set_state(self->pipeline, state);
}

void vlx_pipeline_manager_seek(VlxPipelineManager *self, gint64 position_us) {
    g_return_if_fail(VLX_IS_PIPELINE_MANAGER(self));
    if (!self->pipeline) return;

    g_mutex_lock(&self->seek_mutex);
    self->seek_target_us = position_us;
    g_cond_signal(&self->seek_cond);
    g_mutex_unlock(&self->seek_mutex);
}

gint64 vlx_pipeline_manager_get_position(VlxPipelineManager *self) {
    g_return_val_if_fail(VLX_IS_PIPELINE_MANAGER(self), -1);
    if (!self->pipeline) return -1;

    gint64 pos = -1;
    if (gst_element_query_position(self->pipeline, GST_FORMAT_TIME, &pos))
        return pos / GST_USECOND;
    return -1;
}

gint64 vlx_pipeline_manager_get_duration(VlxPipelineManager *self) {
    g_return_val_if_fail(VLX_IS_PIPELINE_MANAGER(self), 0);
    if (!self->pipeline) return 0;

    gint64 dur = 0;
    if (gst_element_query_duration(self->pipeline, GST_FORMAT_TIME, &dur))
        return dur / GST_USECOND;
    return 0;
}

void vlx_pipeline_manager_set_volume(VlxPipelineManager *self, gdouble volume) {
    g_return_if_fail(VLX_IS_PIPELINE_MANAGER(self));
    if (self->playbin)
        g_object_set(self->playbin, "volume", volume, NULL);
}

void vlx_pipeline_manager_set_muted(VlxPipelineManager *self, gboolean muted) {
    g_return_if_fail(VLX_IS_PIPELINE_MANAGER(self));
    if (self->playbin)
        g_object_set(self->playbin, "mute", muted, NULL);
}

void vlx_pipeline_manager_set_rate(VlxPipelineManager *self, gdouble rate) {
    g_return_if_fail(VLX_IS_PIPELINE_MANAGER(self));
    if (!self->pipeline) return;

    gint64 pos = 0;
    gst_element_query_position(self->pipeline, GST_FORMAT_TIME, &pos);

    if (rate > 0.0) {
        gst_element_seek(self->pipeline, rate, GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT, GST_SEEK_TYPE_SET, pos, GST_SEEK_TYPE_SET, -1);
    }
}

void vlx_pipeline_manager_select_audio(VlxPipelineManager *self, gint index) {
    g_return_if_fail(VLX_IS_PIPELINE_MANAGER(self));
    if (self->playbin)
        g_object_set(self->playbin, "current-audio", index, NULL);
}

void vlx_pipeline_manager_select_subtitle(VlxPipelineManager *self, gint index) {
    g_return_if_fail(VLX_IS_PIPELINE_MANAGER(self));
    if (self->playbin)
        g_object_set(self->playbin, "current-text", index, NULL);
}

void vlx_pipeline_manager_set_stream(VlxPipelineManager *self, const gchar *stream_id) {
    g_return_if_fail(VLX_IS_PIPELINE_MANAGER(self));
    if (!self->pipeline || !self->collection || !self->active_stream_ids) return;

    GList *new_list = NULL;
    for (GList *l = self->active_stream_ids; l != NULL; l = l->next) {
        const gchar *sid = l->data;
        new_list = g_list_append(new_list, g_strdup(sid));
    }
    new_list = g_list_append(new_list, g_strdup(stream_id));

    GstEvent *event = gst_event_new_select_streams(new_list);
    if (!gst_element_send_event(self->pipeline, event)) {
        VLX_LOG_ERROR(VLX_LOG_DOMAIN_PIPELINE, "Failed to send SELECT_STREAMS event");
    } else {
        VLX_LOG_INFO(VLX_LOG_DOMAIN_PIPELINE, "Sent SELECT_STREAMS event for stream %s", stream_id);
    }

    g_list_free_full(new_list, g_free);
}

void vlx_pipeline_manager_set_subtitle_delay(VlxPipelineManager *self, gint64 delay_us) {
    g_return_if_fail(VLX_IS_PIPELINE_MANAGER(self));
    if (self->playbin) {
        g_object_set(self->playbin, "text-offset", delay_us * GST_USECOND, NULL);
    }
}

gint64 vlx_pipeline_manager_get_subtitle_delay(VlxPipelineManager *self) {
    g_return_val_if_fail(VLX_IS_PIPELINE_MANAGER(self), 0);
    gint64 offset = 0;
    if (self->playbin) {
        g_object_get(self->playbin, "text-offset", &offset, NULL);
    }
    return offset / GST_USECOND;
}

GstElement *vlx_pipeline_manager_get_video_sink(VlxPipelineManager *self) {
    g_return_val_if_fail(VLX_IS_PIPELINE_MANAGER(self), NULL);
    return self->video_sink;
}

void vlx_pipeline_manager_set_brightness(VlxPipelineManager *self, gdouble val) {
    g_return_if_fail(VLX_IS_PIPELINE_MANAGER(self));
    if (self->settings) {
        g_settings_set_double(self->settings, "video-brightness", CLAMP(val, -1.0, 1.0));
    }
}

void vlx_pipeline_manager_set_contrast(VlxPipelineManager *self, gdouble val) {
    g_return_if_fail(VLX_IS_PIPELINE_MANAGER(self));
    if (self->settings) {
        g_settings_set_double(self->settings, "video-contrast", CLAMP(val, 0.0, 2.0));
    }
}

void vlx_pipeline_manager_set_saturation(VlxPipelineManager *self, gdouble val) {
    g_return_if_fail(VLX_IS_PIPELINE_MANAGER(self));
    if (self->settings) {
        g_settings_set_double(self->settings, "video-saturation", CLAMP(val, 0.0, 2.0));
    }
}

void vlx_pipeline_manager_set_hue(VlxPipelineManager *self, gdouble val) {
    g_return_if_fail(VLX_IS_PIPELINE_MANAGER(self));
    if (self->settings) {
        g_settings_set_double(self->settings, "video-hue", CLAMP(val, -1.0, 1.0));
    }
}

void vlx_pipeline_manager_load_subtitle_file(VlxPipelineManager *self, const gchar *path) {
    g_return_if_fail(VLX_IS_PIPELINE_MANAGER(self));
    g_return_if_fail(path != NULL);
    if (!self->playbin) return;

    gchar *sub_uri = gst_filename_to_uri(path, NULL);
    if (!sub_uri) return;

    VLX_LOG_INFO(VLX_LOG_DOMAIN_PIPELINE, "Loading external subtitle: %s", sub_uri);
    g_object_set(self->playbin, "suburi", sub_uri, NULL);
    g_free(sub_uri);
}
