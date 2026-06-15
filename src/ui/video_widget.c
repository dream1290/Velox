/* vlx_video_widget.c — Zero-copy video renderer using GtkPicture and GdkTexture
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ui/video_widget.h"
#include "utils/log.h"
#include <gst/gst.h>
#include <gst/video/video.h>

struct _VlxVideoWidget {
    GtkWidget  parent_instance;

    GtkWidget *picture;       /* GtkPicture child */

    GstElement *sink;         /* appsink */
    gint        disposed;

    /* Screenshot request */
    gchar      *screenshot_path;
    GMutex      state_lock;
    GdkTexture *current_texture;
};

G_DEFINE_TYPE (VlxVideoWidget, vlx_video_widget, GTK_TYPE_WIDGET)

/* ── Zero-copy buffer management ─────────────────────────────────────── */
typedef struct {
    GstBuffer     *buf;
    GstVideoFrame  frame;
} FrameData;

static void
frame_data_free (gpointer data)
{
    FrameData *fd = data;
    gst_video_frame_unmap (&fd->frame);
    gst_buffer_unref (fd->buf);
    g_free (fd);
}

/* ── Idle render callback (Main Thread) ──────────────────────────────── */
static gboolean
update_texture_idle (gpointer data)
{
    GdkTexture *tex = GDK_TEXTURE (data);
    VlxVideoWidget *self = g_object_get_data (G_OBJECT (tex), "widget");

    if (self && !g_atomic_int_get (&self->disposed)) {
        g_mutex_lock (&self->state_lock);
        g_clear_object (&self->current_texture);
        self->current_texture = g_object_ref (tex);
        g_mutex_unlock (&self->state_lock);

        gtk_picture_set_paintable (GTK_PICTURE (self->picture), GDK_PAINTABLE (tex));

        /* Handle screenshot if requested */
        g_mutex_lock (&self->state_lock);
        if (self->screenshot_path) {
            GError *err = NULL;
            if (!gdk_texture_save_to_png (tex, self->screenshot_path)) {
                VLX_LOG_ERROR (VLX_LOG_DOMAIN_UI, "Failed to save screenshot: %s", err ? err->message : "unknown error");
                g_clear_error (&err);
            } else {
                VLX_LOG_INFO (VLX_LOG_DOMAIN_UI, "Saved screenshot to %s", self->screenshot_path);
            }
            g_clear_pointer (&self->screenshot_path, g_free);
        }
        g_mutex_unlock (&self->state_lock);
    }

    g_object_unref (tex);
    if (self) g_object_unref (self);
    return G_SOURCE_REMOVE;
}

/* ── GStreamer "new-sample" callback ─────────────────────────────────── */
static GstFlowReturn
on_new_sample (GstElement *sink, gpointer data)
{
    VlxVideoWidget *self = VLX_VIDEO_WIDGET (data);

    if (g_atomic_int_get (&self->disposed))
        return GST_FLOW_EOS;

    GstSample *sample = NULL;
    g_signal_emit_by_name (sink, "pull-sample", &sample);
    if (!sample) return GST_FLOW_ERROR;

    GstBuffer *buf = gst_sample_get_buffer (sample);
    if (!buf) {
        gst_sample_unref (sample);
        return GST_FLOW_OK;
    }

    GstCaps *caps = gst_sample_get_caps (sample);
    GstVideoInfo info;
    if (!gst_video_info_from_caps (&info, caps)) {
        gst_sample_unref (sample);
        return GST_FLOW_ERROR;
    }

    /* Wrap the buffer into a zero-copy GdkTexture */
    FrameData *fd = g_new0 (FrameData, 1);
    fd->buf = gst_buffer_ref (buf);

    if (!gst_video_frame_map (&fd->frame, &info, fd->buf, GST_MAP_READ)) {
        gst_buffer_unref (fd->buf);
        g_free (fd);
        gst_sample_unref (sample);
        return GST_FLOW_ERROR;
    }

    gsize stride = GST_VIDEO_FRAME_PLANE_STRIDE (&fd->frame, 0);
    gsize size = stride * info.height;
    guint8 *pixels = GST_VIDEO_FRAME_PLANE_DATA (&fd->frame, 0);

    GBytes *bytes = g_bytes_new_with_free_func (pixels, size, frame_data_free, fd);
    GdkTexture *tex = gdk_memory_texture_new (info.width, info.height,
                                              GDK_MEMORY_R8G8B8A8,
                                              bytes, stride);
    g_bytes_unref (bytes);
    gst_sample_unref (sample);

    if (!tex) {
        VLX_LOG_WARNING (VLX_LOG_DOMAIN_UI, "Failed to create GdkTexture");
        return GST_FLOW_ERROR;
    }

    /* Schedule UI update */
    g_object_ref (self);
    g_object_set_data (G_OBJECT (tex), "widget", self);
    g_idle_add (update_texture_idle, tex);

    return GST_FLOW_OK;
}

/* ── GObject boilerplate ─────────────────────────────────────────────── */
static void
vlx_video_widget_dispose (GObject *obj)
{
    VlxVideoWidget *self = VLX_VIDEO_WIDGET (obj);
    g_atomic_int_set (&self->disposed, TRUE);

    if (self->sink) {
        g_signal_handlers_disconnect_by_data (self->sink, self);
        g_clear_object (&self->sink);
    }

    if (self->picture) {
        gtk_widget_unparent (self->picture);
        self->picture = NULL;
    }

    g_mutex_lock (&self->state_lock);
    g_clear_object (&self->current_texture);
    g_clear_pointer (&self->screenshot_path, g_free);
    g_mutex_unlock (&self->state_lock);

    G_OBJECT_CLASS (vlx_video_widget_parent_class)->dispose (obj);
}

static void
vlx_video_widget_finalize (GObject *obj)
{
    VlxVideoWidget *self = VLX_VIDEO_WIDGET (obj);
    g_mutex_clear (&self->state_lock);
    G_OBJECT_CLASS (vlx_video_widget_parent_class)->finalize (obj);
}

static void
vlx_video_widget_class_init (VlxVideoWidgetClass *klass)
{
    GObjectClass *obj_class = G_OBJECT_CLASS (klass);
    obj_class->dispose = vlx_video_widget_dispose;
    obj_class->finalize = vlx_video_widget_finalize;

    /* Important: Must set layout manager so the child picture fills the widget */
    gtk_widget_class_set_layout_manager_type (GTK_WIDGET_CLASS (klass), GTK_TYPE_BIN_LAYOUT);
}

static void
vlx_video_widget_init (VlxVideoWidget *self)
{
    g_mutex_init (&self->state_lock);

    /* Setup the internal GtkPicture */
    self->picture = gtk_picture_new ();
    gtk_picture_set_can_shrink (GTK_PICTURE (self->picture), TRUE);
    gtk_picture_set_content_fit (GTK_PICTURE (self->picture), GTK_CONTENT_FIT_CONTAIN);
    gtk_widget_set_parent (self->picture, GTK_WIDGET (self));

    gtk_widget_set_hexpand (GTK_WIDGET (self), TRUE);
    gtk_widget_set_vexpand (GTK_WIDGET (self), TRUE);
}

/* ── Public API ──────────────────────────────────────────────────────── */
VlxVideoWidget *
vlx_video_widget_new (void)
{
    return g_object_new (VLX_TYPE_VIDEO_WIDGET, NULL);
}

void
vlx_video_widget_set_sink (VlxVideoWidget *self, GstElement *sink)
{
    g_return_if_fail (VLX_IS_VIDEO_WIDGET (self));

    if (self->sink) {
        g_signal_handlers_disconnect_by_data (self->sink, self);
        g_object_unref (self->sink);
        self->sink = NULL;
    }

    if (sink) {
        self->sink = g_object_ref (sink);
        g_object_set (self->sink, "emit-signals", TRUE, NULL);
        g_signal_connect (self->sink, "new-sample",
                          G_CALLBACK (on_new_sample), self);
    }
}

void
vlx_video_widget_take_screenshot (VlxVideoWidget *self, const gchar *path)
{
    g_return_if_fail (VLX_IS_VIDEO_WIDGET (self));
    g_return_if_fail (path != NULL);

    g_mutex_lock (&self->state_lock);
    
    /* If we already have a texture, save it immediately */
    if (self->current_texture) {
        GError *err = NULL;
        if (!gdk_texture_save_to_png (self->current_texture, path)) {
            VLX_LOG_ERROR (VLX_LOG_DOMAIN_UI, "Failed to save screenshot immediately: %s", err ? err->message : "unknown error");
            g_clear_error (&err);
        } else {
            VLX_LOG_INFO (VLX_LOG_DOMAIN_UI, "Saved screenshot to %s immediately", path);
        }
    } else {
        g_free (self->screenshot_path);
        self->screenshot_path = g_strdup (path);
    }

    g_mutex_unlock (&self->state_lock);
}

GdkTexture *
vlx_video_widget_get_current_texture (VlxVideoWidget *self)
{
    g_return_val_if_fail (VLX_IS_VIDEO_WIDGET (self), NULL);

    g_mutex_lock (&self->state_lock);
    GdkTexture *tex = self->current_texture ? g_object_ref (self->current_texture) : NULL;
    g_mutex_unlock (&self->state_lock);

    return tex;
}
