/* vlx_subtitle_overlay.c — Pango text overlay for subtitle rendering
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Renders a single subtitle string at the bottom-centre of the video
 * using Pango markup (bold, italic, colour tags from ASS/SRT HTML).
 * Uses GTK's snapshot→Cairo path so it composites correctly over GLArea.
 */

#include "ui/subtitle_overlay.h"

#include <pango/pangocairo.h>

struct _VlxSubtitleOverlay {
    GtkWidget parent_instance;
    gchar    *markup;   /* current subtitle text (Pango markup) */
};

G_DEFINE_TYPE (VlxSubtitleOverlay, vlx_subtitle_overlay, GTK_TYPE_WIDGET)

static void
vlx_subtitle_overlay_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
    VlxSubtitleOverlay *self = VLX_SUBTITLE_OVERLAY (widget);

    if (!self->markup || self->markup[0] == '\0')
        return;

    gdouble w = gtk_widget_get_width (widget);
    gdouble h = gtk_widget_get_height (widget);

    cairo_t *cr = gtk_snapshot_append_cairo (snapshot,
        &GRAPHENE_RECT_INIT (0, 0, w, h));

    PangoLayout *layout = pango_cairo_create_layout (cr);

    PangoFontDescription *font = pango_font_description_from_string ("Sans Bold 20");
    pango_layout_set_font_description (layout, font);
    pango_font_description_free (font);

    pango_layout_set_markup (layout, self->markup, -1);
    pango_layout_set_width  (layout, (int) (w * 0.85 * PANGO_SCALE));
    pango_layout_set_alignment (layout, PANGO_ALIGN_CENTER);
    pango_layout_set_wrap (layout, PANGO_WRAP_WORD_CHAR);

    int pw, ph;
    pango_layout_get_pixel_size (layout, &pw, &ph);

    gdouble tx = (w - pw) / 2.0;
    gdouble ty = h - ph - 32.0;    /* 32 px margin from bottom */

    /* Semi-transparent pill background */
    double pad = 8.0, radius = 6.0;
    cairo_new_sub_path (cr);
    cairo_arc (cr, tx - pad + radius,      ty - pad + radius,      radius, G_PI, 3.0 * G_PI / 2.0);
    cairo_arc (cr, tx + pw + pad - radius, ty - pad + radius,      radius, 3.0 * G_PI / 2.0, 0);
    cairo_arc (cr, tx + pw + pad - radius, ty + ph + pad - radius, radius, 0, G_PI / 2.0);
    cairo_arc (cr, tx - pad + radius,      ty + ph + pad - radius, radius, G_PI / 2.0, G_PI);
    cairo_close_path (cr);
    cairo_set_source_rgba (cr, 0.0, 0.0, 0.0, 0.65);
    cairo_fill (cr);

    /* Text */
    cairo_move_to (cr, tx, ty);
    cairo_set_source_rgba (cr, 1.0, 1.0, 1.0, 1.0);
    pango_cairo_show_layout (cr, layout);

    g_object_unref (layout);
    cairo_destroy (cr);
}

static void
vlx_subtitle_overlay_finalize (GObject *obj)
{
    VlxSubtitleOverlay *self = VLX_SUBTITLE_OVERLAY (obj);
    g_free (self->markup);
    G_OBJECT_CLASS (vlx_subtitle_overlay_parent_class)->finalize (obj);
}

static void
vlx_subtitle_overlay_class_init (VlxSubtitleOverlayClass *klass)
{
    G_OBJECT_CLASS   (klass)->finalize = vlx_subtitle_overlay_finalize;
    GTK_WIDGET_CLASS (klass)->snapshot = vlx_subtitle_overlay_snapshot;
}

static void
vlx_subtitle_overlay_init (VlxSubtitleOverlay *self)
{
    gtk_widget_set_can_target   (GTK_WIDGET (self), FALSE);
    gtk_widget_set_hexpand      (GTK_WIDGET (self), TRUE);
    gtk_widget_set_vexpand      (GTK_WIDGET (self), TRUE);
}

VlxSubtitleOverlay *
vlx_subtitle_overlay_new (void)
{
    return g_object_new (VLX_TYPE_SUBTITLE_OVERLAY, NULL);
}

void
vlx_subtitle_overlay_set_text (VlxSubtitleOverlay *self, const gchar *text)
{
    g_return_if_fail (VLX_IS_SUBTITLE_OVERLAY (self));

    g_free (self->markup);
    self->markup = text ? g_strdup (text) : NULL;
    gtk_widget_queue_draw (GTK_WIDGET (self));
}
