/* vlx_video_widget.h — GtkGLArea subclass for zero-copy video
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gtk/gtk.h>
#include <gst/gst.h>

G_BEGIN_DECLS

#define VLX_TYPE_VIDEO_WIDGET (vlx_video_widget_get_type ())
G_DECLARE_FINAL_TYPE (VlxVideoWidget, vlx_video_widget,
                      VLX, VIDEO_WIDGET, GtkGLArea)

VlxVideoWidget *vlx_video_widget_new (void);

void vlx_video_widget_set_sink (VlxVideoWidget *self,
                                GstElement     *sink);

void vlx_video_widget_take_screenshot (VlxVideoWidget *self,
                                       const gchar    *path);

/* Returns a GdkTexture snapshot of the current frame.
 * Caller owns the returned reference (use g_object_unref). */
GdkTexture *vlx_video_widget_get_current_texture (VlxVideoWidget *self);

G_END_DECLS
