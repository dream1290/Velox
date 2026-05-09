/* vlx_pipeline.h — GStreamer pipeline lifecycle (L3)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <glib-object.h>
#include <gst/gst.h>

G_BEGIN_DECLS

#define VLX_TYPE_PIPELINE_MANAGER (vlx_pipeline_manager_get_type ())
G_DECLARE_FINAL_TYPE (VlxPipelineManager, vlx_pipeline_manager,
                      VLX, PIPELINE_MANAGER, GObject)

/* Callback typedefs for player ↔ pipeline communication */
typedef void (*VlxPipelineStateCb)     (VlxPipelineManager *pm,
                                        GstState old_st, GstState new_st,
                                        gpointer data);
typedef void (*VlxPipelineEosCb)       (VlxPipelineManager *pm,
                                        gpointer data);
typedef void (*VlxPipelineErrorCb)     (VlxPipelineManager *pm,
                                        const gchar *msg,
                                        gpointer data);
typedef void (*VlxPipelineBufferingCb) (VlxPipelineManager *pm,
                                        gint percent,
                                        gpointer data);
typedef void (*VlxPipelineCollectionCb) (VlxPipelineManager *pm,
                                         GstStreamCollection *collection,
                                         gpointer data);
typedef void (*VlxPipelineTocCb)        (VlxPipelineManager *pm,
                                         GstToc             *toc,
                                         gpointer            data);

VlxPipelineManager *vlx_pipeline_manager_new (void);

void vlx_pipeline_manager_set_callbacks (VlxPipelineManager    *self,
                                         VlxPipelineStateCb     state_cb,
                                         VlxPipelineEosCb       eos_cb,
                                         VlxPipelineErrorCb     error_cb,
                                         VlxPipelineBufferingCb buf_cb,
                                         VlxPipelineCollectionCb coll_cb,
                                         VlxPipelineTocCb        toc_cb,
                                         gpointer               data);

void vlx_pipeline_manager_open      (VlxPipelineManager *self,
                                     const gchar        *uri);
void vlx_pipeline_manager_set_state (VlxPipelineManager *self,
                                     GstState            state);
void vlx_pipeline_manager_seek      (VlxPipelineManager *self,
                                     gint64              position_us);

gint64 vlx_pipeline_manager_get_position (VlxPipelineManager *self);
gint64 vlx_pipeline_manager_get_duration (VlxPipelineManager *self);

void vlx_pipeline_manager_set_volume (VlxPipelineManager *self,
                                      gdouble volume);
void vlx_pipeline_manager_set_muted  (VlxPipelineManager *self,
                                      gboolean muted);
void vlx_pipeline_manager_set_rate   (VlxPipelineManager *self,
                                      gdouble rate);

void vlx_pipeline_manager_select_audio    (VlxPipelineManager *self,
                                           gint index);
void vlx_pipeline_manager_select_subtitle (VlxPipelineManager *self,
                                           gint index);

void vlx_pipeline_manager_set_stream      (VlxPipelineManager *self,
                                           const gchar *stream_id);

void vlx_pipeline_manager_set_subtitle_delay (VlxPipelineManager *self,
                                              gint64 delay_us);
gint64 vlx_pipeline_manager_get_subtitle_delay (VlxPipelineManager *self);

/**
 * vlx_pipeline_manager_get_video_sink:
 *
 * Returns: (transfer none)(nullable): The video sink element,
 *          needed by VlxVideoWidget to share the GL context.
 */
GstElement *vlx_pipeline_manager_get_video_sink (VlxPipelineManager *self);

/* Video balance (brightness/contrast/saturation: -1.0..1.0, hue: -1.0..1.0) */
void vlx_pipeline_manager_set_brightness (VlxPipelineManager *self, gdouble val);
void vlx_pipeline_manager_set_contrast   (VlxPipelineManager *self, gdouble val);
void vlx_pipeline_manager_set_saturation (VlxPipelineManager *self, gdouble val);
void vlx_pipeline_manager_set_hue        (VlxPipelineManager *self, gdouble val);

/* External subtitle file */
void vlx_pipeline_manager_load_subtitle_file (VlxPipelineManager *self,
                                              const gchar        *path);

G_END_DECLS
