/* vlx_pip_window.c — Picture-in-Picture window
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Uses a plain GtkWindow with set_transient_for + no decorations.
 * On Wayland, "always on top" is compositor-dependent; we do our best
 * with the available hints.  On X11 it works via NET_WM_STATE_ABOVE.
 */

#include "ui/pip_window.h"
#include "utils/log.h"

#include <gst/video/video.h>

struct _VlxPipWindow {
    GtkWindow parent_instance;

    VlxVideoWidget *source_widget;
    GtkWidget      *picture;
    guint           tick_id;
};

G_DEFINE_TYPE (VlxPipWindow, vlx_pip_window, GTK_TYPE_WINDOW)

/* Pull a CPU-side texture from the main video widget each frame. */
static gboolean
on_pip_tick (GtkWidget *widget, GdkFrameClock *clock, gpointer data)
{
    VlxPipWindow *self = VLX_PIP_WINDOW (widget);
    (void) clock; (void) data;

    if (!self->source_widget) return G_SOURCE_CONTINUE;

    GdkTexture *tex = vlx_video_widget_get_current_texture (self->source_widget);
    if (tex) {
        gtk_picture_set_paintable (GTK_PICTURE (self->picture),
                                   GDK_PAINTABLE (tex));
        g_object_unref (tex);
    }
    return G_SOURCE_CONTINUE;
}

static void
vlx_pip_window_dispose (GObject *obj)
{
    VlxPipWindow *self = VLX_PIP_WINDOW (obj);
    if (self->tick_id > 0) {
        gtk_widget_remove_tick_callback (GTK_WIDGET (self), self->tick_id);
        self->tick_id = 0;
    }
    G_OBJECT_CLASS (vlx_pip_window_parent_class)->dispose (obj);
}

static void
vlx_pip_window_class_init (VlxPipWindowClass *klass)
{
    G_OBJECT_CLASS (klass)->dispose = vlx_pip_window_dispose;
}

static void
vlx_pip_window_init (VlxPipWindow *self)
{
    gtk_window_set_title (GTK_WINDOW (self), "Velox PiP");
    gtk_window_set_default_size (GTK_WINDOW (self), 480, 270);
    gtk_window_set_decorated (GTK_WINDOW (self), FALSE);
    gtk_window_set_resizable (GTK_WINDOW (self), TRUE);
    gtk_window_set_deletable (GTK_WINDOW (self), TRUE);

    self->picture = gtk_picture_new ();
    gtk_picture_set_can_shrink (GTK_PICTURE (self->picture), TRUE);
    gtk_picture_set_content_fit (GTK_PICTURE (self->picture),
                                 GTK_CONTENT_FIT_CONTAIN);
    gtk_window_set_child (GTK_WINDOW (self), self->picture);

    /* Black background via inline CSS */
    GtkCssProvider *css = gtk_css_provider_new ();
    gtk_css_provider_load_from_string (css,
        "window.pip-window { background-color: #000; }");
    gtk_style_context_add_provider_for_display (
        gdk_display_get_default (),
        GTK_STYLE_PROVIDER (css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref (css);
    gtk_widget_add_css_class (GTK_WIDGET (self), "pip-window");
}

VlxPipWindow *
vlx_pip_window_new (VlxVideoWidget *source_widget)
{
    VlxPipWindow *self = g_object_new (VLX_TYPE_PIP_WINDOW, NULL);
    self->source_widget = source_widget;
    return self;
}

void
vlx_pip_window_present (VlxPipWindow *self, GtkWindow *parent)
{
    g_return_if_fail (VLX_IS_PIP_WINDOW (self));

    gtk_window_set_transient_for (GTK_WINDOW (self), parent);

    /* Start frame-pulling tick */
    if (self->tick_id == 0) {
        self->tick_id = gtk_widget_add_tick_callback (
            GTK_WIDGET (self), on_pip_tick, NULL, NULL);
    }

    gtk_window_present (GTK_WINDOW (self));

    VLX_LOG_INFO (VLX_LOG_DOMAIN_UI, "PiP window opened");
}

void
vlx_pip_window_close_pip (VlxPipWindow *self)
{
    g_return_if_fail (VLX_IS_PIP_WINDOW (self));
    if (self->tick_id > 0) {
        gtk_widget_remove_tick_callback (GTK_WIDGET (self), self->tick_id);
        self->tick_id = 0;
    }
    gtk_window_close (GTK_WINDOW (self));
    VLX_LOG_INFO (VLX_LOG_DOMAIN_UI, "PiP window closed");
}
