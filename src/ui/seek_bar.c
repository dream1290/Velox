/* vlx_seek_bar.c — Custom Cairo-drawn seek bar
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Features:
 *   • Dual-colour track: played (accent) / remaining (dim)
 *   • Buffered range indicator (translucent accent)
 *   • Chapter tick marks
 *   • Draggable thumb with smooth hover expand animation
 *   • Time tooltip on hover
 */

#include "ui/seek_bar.h"
#include "utils/log.h"
#include "media/thumbnail.h"

#include <cairo.h>
#include <math.h>

#define BAR_HEIGHT     4.0
#define THUMB_RADIUS   7.0
#define THUMB_HOVER_R  9.0
#define CHAPTER_H      8.0
#define WIDGET_HEIGHT  36.0   /* taller hit target */

/* Thumbnail popup size */
#define THUMB_W        160
#define THUMB_H        90
#define THUMB_R        6.0    /* rounded corner radius */
#define THUMB_PAD      6.0    /* gap between thumb and label pill */

/* How many µs between thumbnail requests while hovering */
#define THUMB_REQUEST_INTERVAL_US  (2 * G_USEC_PER_SEC)

typedef struct {
    gdouble      fraction;
    gchar       *label;
} Chapter;

struct _VlxSeekBar {
    GtkWidget parent_instance;

    gdouble   position;    /* 0.0 – 1.0 */
    gdouble   buffered;    /* 0.0 – 1.0 */
    gint64    duration_us;

    gboolean  dragging;
    gdouble   hover_x;     /* < 0 = not hovering */
    gdouble   thumb_r;     /* animated thumb radius */

    gchar             *uri;
    gint64             last_requested_us;
    VlxThumbnailCache *thumb_cache;
    GdkTexture        *hover_texture;

    gdouble   drag_start_x;
    gint64    last_scrub_time;

    GArray   *chapters;    /* Chapter structs */

    /* A-B loop markers */
    gdouble   ab_a;        /* fraction, < 0 = not set */
    gdouble   ab_b;
    gboolean  ab_active;

    /* Signals */
    guint     seek_signal;

    PangoLayout *pill_layout;
};

G_DEFINE_TYPE (VlxSeekBar, vlx_seek_bar, GTK_TYPE_WIDGET)

enum {
    SIGNAL_SEEK_REQUESTED,
    N_SIGNALS,
};
static guint seek_signals[N_SIGNALS];

/* ── Helpers ─────────────────────────────────────────────────────────── */
static gdouble
x_to_fraction (VlxSeekBar *self, gdouble x)
{
    gdouble w = gtk_widget_get_width (GTK_WIDGET (self));
    return CLAMP (x / w, 0.0, 1.0);
}

static gchar *
format_time (gint64 us)
{
    gint64 secs  = us / G_USEC_PER_SEC;
    gint   h     = (gint) (secs / 3600);
    gint   m     = (gint) ((secs % 3600) / 60);
    gint   s     = (gint) (secs % 60);

    if (h > 0)
        return g_strdup_printf ("%d:%02d:%02d", h, m, s);
    return g_strdup_printf ("%d:%02d", m, s);
}

/* ── Drawing ─────────────────────────────────────────────────────────── */
static void
vlx_seek_bar_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
    VlxSeekBar *self = VLX_SEEK_BAR (widget);
    gdouble w  = gtk_widget_get_width (widget);
    gdouble h  = gtk_widget_get_height (widget);
    gdouble cy = h / 2.0;

    /* Allow drawing above the widget for the thumbnail popup */
    gdouble popup_h = THUMB_H + THUMB_PAD + 22.0 + THUMB_HOVER_R + 10.0;
    graphene_rect_t full_rect = GRAPHENE_RECT_INIT (0, -popup_h, w, h + popup_h);
    cairo_t *cr = gtk_snapshot_append_cairo (snapshot, &full_rect);

    /* ── Track background ─────────────────────────────────────────── */
    cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);

    /* Remaining portion */
    cairo_set_source_rgba (cr, 1.0, 1.0, 1.0, 0.2);
    cairo_rectangle (cr, 0, cy - BAR_HEIGHT / 2.0, w, BAR_HEIGHT);
    cairo_fill (cr);

    /* Buffered range */
    if (self->buffered > 0.0) {
        cairo_set_source_rgba (cr, 1.0, 1.0, 1.0, 0.35);
        cairo_rectangle (cr, 0, cy - BAR_HEIGHT / 2.0,
                         self->buffered * w, BAR_HEIGHT);
        cairo_fill (cr);
    }

    /* Played range — accent colour */
    cairo_set_source_rgba (cr, 0.37, 0.71, 1.0, 1.0);   /* #5EB5FF */
    cairo_rectangle (cr, 0, cy - BAR_HEIGHT / 2.0,
                     self->position * w, BAR_HEIGHT);
    cairo_fill (cr);

    /* ── Chapter tick marks ───────────────────────────────────────── */
    cairo_set_source_rgba (cr, 1.0, 1.0, 1.0, 0.8);
    for (guint i = 0; i < self->chapters->len; i++) {
        Chapter *ch = &g_array_index (self->chapters, Chapter, i);
        gdouble cx = ch->fraction * w;
        cairo_rectangle (cr, cx - 1.0, cy - CHAPTER_H / 2.0, 2.0, CHAPTER_H);
        cairo_fill (cr);
    }

    /* ── A-B loop markers ─────────────────────────────────────────── */
    if (self->ab_a >= 0.0) {
        gdouble ax = self->ab_a * w;

        /* If B is set, shade the A-B region */
        if (self->ab_b >= 0.0 && self->ab_active) {
            gdouble bx = self->ab_b * w;
            cairo_set_source_rgba (cr, 1.0, 0.42, 0.42, 0.18); /* red tint */
            cairo_rectangle (cr, ax, cy - CHAPTER_H / 2.0,
                             bx - ax, CHAPTER_H);
            cairo_fill (cr);
        }

        /* A pin — red triangle/diamond */
        cairo_set_source_rgba (cr, 1.0, 0.35, 0.35, 1.0);
        cairo_move_to (cr, ax, cy - CHAPTER_H / 2.0 - 2.0);
        cairo_line_to (cr, ax + 4.0, cy);
        cairo_line_to (cr, ax, cy + CHAPTER_H / 2.0 + 2.0);
        cairo_line_to (cr, ax - 4.0, cy);
        cairo_close_path (cr);
        cairo_fill (cr);

        /* B pin */
        if (self->ab_b >= 0.0) {
            gdouble bx = self->ab_b * w;
            cairo_set_source_rgba (cr, 0.35, 0.55, 1.0, 1.0);
            cairo_move_to (cr, bx, cy - CHAPTER_H / 2.0 - 2.0);
            cairo_line_to (cr, bx + 4.0, cy);
            cairo_line_to (cr, bx, cy + CHAPTER_H / 2.0 + 2.0);
            cairo_line_to (cr, bx - 4.0, cy);
            cairo_close_path (cr);
            cairo_fill (cr);
        }
    }

    /* ── Hover position indicator line ───────────────────────────────── */
    if (self->hover_x >= 0 && self->duration_us > 0) {
        cairo_set_source_rgba (cr, 1.0, 1.0, 1.0, 0.35);
        cairo_set_line_width (cr, 1.0);
        cairo_move_to (cr, self->hover_x, cy - BAR_HEIGHT / 2.0 - 2.0);
        cairo_line_to (cr, self->hover_x, cy + BAR_HEIGHT / 2.0 + 2.0);
        cairo_stroke (cr);
    }

    /* ── Thumb ────────────────────────────────────────────────────────── */
    gdouble tx = self->position * w;
    gdouble tr = (self->hover_x >= 0 || self->dragging)
                 ? THUMB_HOVER_R : THUMB_RADIUS;

    /* Drop shadow */
    cairo_set_source_rgba (cr, 0.0, 0.0, 0.0, 0.25);
    cairo_arc (cr, tx, cy + 1.0, tr, 0, 2.0 * G_PI);
    cairo_fill (cr);

    /* Thumb body */
    cairo_set_source_rgba (cr, 1.0, 1.0, 1.0, 1.0);
    cairo_arc (cr, tx, cy, tr, 0, 2.0 * G_PI);
    cairo_fill (cr);

    /* Accent inner dot */
    cairo_set_source_rgba (cr, 0.37, 0.71, 1.0, 1.0);
    cairo_arc (cr, tx, cy, tr * 0.4, 0, 2.0 * G_PI);
    cairo_fill (cr);

    /* ── Hover popup: thumbnail preview + time pill ───────────────────── */
    if (self->hover_x >= 0 && self->duration_us > 0) {
        gdouble frac = x_to_fraction (self, self->hover_x);
        gint64  t_us = (gint64) (frac * self->duration_us);
        gchar  *ts   = format_time (t_us);

        /* Popup anchor: bottom of thumbnail sits above the thumb knob */
        gdouble pop_x      = CLAMP (self->hover_x - THUMB_W / 2.0,
                                     4.0, w - THUMB_W - 4.0);
        gdouble pop_bottom = cy - THUMB_HOVER_R - 8.0;   /* in widget coords */
        gdouble pop_top    = pop_bottom - THUMB_H;       /* negative = above widget */

        if (self->hover_texture) {
            /* Finish cairo before appending textures */
            cairo_destroy (cr);
            cr = NULL;

            /* Soft shadow behind the thumbnail card */
            graphene_rect_t shadow_r = GRAPHENE_RECT_INIT (
                pop_x - 4, pop_top - 4, THUMB_W + 8, THUMB_H + 8);
            gtk_snapshot_append_color (snapshot,
                &(GdkRGBA){0.0, 0.0, 0.0, 0.55}, &shadow_r);

            /* Thumbnail image */
            graphene_rect_t thumb_r = GRAPHENE_RECT_INIT (
                pop_x, pop_top, THUMB_W, THUMB_H);
            gtk_snapshot_append_texture (snapshot, self->hover_texture, &thumb_r);

            /* Re-open cairo for the time pill */
            cr = gtk_snapshot_append_cairo (snapshot, &full_rect);
        }

        if (!cr)
            cr = gtk_snapshot_append_cairo (snapshot, &full_rect);

        /* Time pill: just below the thumbnail (or above thumb if no texture) */
        pango_layout_set_text (self->pill_layout, ts, -1);

        gint lw, lh;
        pango_layout_get_pixel_size (self->pill_layout, &lw, &lh);

        gdouble label_cx = CLAMP (self->hover_x - lw / 2.0,
                                   4.0, w - lw - 4.0);
        /* When texture is present, label goes at thumbnail bottom + gap;
           otherwise it floats directly above the thumb */
        gdouble label_base = self->hover_texture
                             ? pop_bottom - 4.0
                             : pop_bottom;

        /* Rounded pill background */
        double pad = 5.0, rc = 4.0;
        double rx = label_cx - pad;
        double ry = label_base - lh - pad;
        double rw = lw + pad * 2.0;
        double rh = lh + pad * 2.0;
        cairo_new_sub_path (cr);
        cairo_arc (cr, rx + rc,      ry + rc,      rc, G_PI,      3*G_PI/2);
        cairo_arc (cr, rx + rw - rc, ry + rc,      rc, 3*G_PI/2,  2*G_PI);
        cairo_arc (cr, rx + rw - rc, ry + rh - rc, rc, 0,         G_PI/2);
        cairo_arc (cr, rx + rc,      ry + rh - rc, rc, G_PI/2,    G_PI);
        cairo_close_path (cr);
        cairo_set_source_rgba (cr, 0.07, 0.07, 0.07, 0.92);
        cairo_fill (cr);
        
        cairo_destroy (cr);
        cr = NULL; /* done with cairo for now */

        /* Draw the text layout */
        gtk_snapshot_save (snapshot);
        gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (label_cx, label_base - lh));
        gtk_snapshot_append_layout (snapshot, self->pill_layout, &(GdkRGBA){1.0, 1.0, 1.0, 1.0});
        gtk_snapshot_restore (snapshot);

        g_free (ts);
    }

    if (cr)
        cairo_destroy (cr);
}


/* ── Gesture / motion ────────────────────────────────────────────────── */
static void
on_drag_begin (GtkGestureDrag *g, gdouble x, gdouble y, gpointer data)
{
    VlxSeekBar *self = VLX_SEEK_BAR (data);
    (void) g; (void) y;
    self->dragging = TRUE;
    self->drag_start_x = x;
    self->position = x_to_fraction (self, x);
    gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
on_drag_update (GtkGestureDrag *g, gdouble dx, gdouble dy, gpointer data)
{
    VlxSeekBar *self = VLX_SEEK_BAR (data);
    (void) g; (void) dy;
    self->position = x_to_fraction (self, self->drag_start_x + dx);
    gtk_widget_queue_draw (GTK_WIDGET (self));

    /* Live scrub with 100ms throttle */
    gint64 now = g_get_monotonic_time ();
    if (now - self->last_scrub_time >= 100000) {
        self->last_scrub_time = now;
        g_signal_emit (self, seek_signals[SIGNAL_SEEK_REQUESTED], 0, self->position);
    }
}

static void
on_drag_end (GtkGestureDrag *g, gdouble dx, gdouble dy, gpointer data)
{
    VlxSeekBar *self = VLX_SEEK_BAR (data);
    (void) g; (void) dx; (void) dy;
    self->dragging = FALSE;
    g_signal_emit (self, seek_signals[SIGNAL_SEEK_REQUESTED], 0,
                   self->position);
    gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
on_thumbnail_ready (const gchar *uri, gint64 position_us, GdkTexture *texture, gpointer user_data)
{
    VlxSeekBar *self = VLX_SEEK_BAR (user_data);
    if (g_strcmp0 (self->uri, uri) == 0 && texture && position_us == self->last_requested_us) {
        g_set_object (&self->hover_texture, texture);
        gtk_widget_queue_draw (GTK_WIDGET (self));
    }
    g_object_unref (self);
}

static void
on_motion (GtkEventControllerMotion *m,
           gdouble x, gdouble y,
           gpointer data)
{
    VlxSeekBar *self = VLX_SEEK_BAR (data);
    (void) m; (void) y;
    self->hover_x = x;

    if (self->uri && self->duration_us > 0) {
        gdouble frac     = x_to_fraction (self, x);
        gint64  hover_us = (gint64) (frac * self->duration_us);
        /* Snap to 2-second intervals to maximise cache reuse */
        gint64  snapped  = (hover_us / (2 * G_USEC_PER_SEC)) * (2 * G_USEC_PER_SEC);
        if (ABS (snapped - self->last_requested_us) >= THUMB_REQUEST_INTERVAL_US) {
            self->last_requested_us = snapped;
            g_clear_object (&self->hover_texture);   /* clear stale frame */
            g_object_ref (self);
            vlx_thumbnail_cache_request (self->thumb_cache, self->uri, snapped,
                                         on_thumbnail_ready, self);
        }
    }
    
    gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
on_leave (GtkEventControllerMotion *m, gpointer data)
{
    VlxSeekBar *self = VLX_SEEK_BAR (data);
    (void) m;
    self->hover_x = -1.0;
    self->last_requested_us = -1;
    g_clear_object (&self->hover_texture);
    gtk_widget_queue_draw (GTK_WIDGET (self));
}

/* ── GObject boilerplate ─────────────────────────────────────────────── */
static void
chapter_clear (gpointer data)
{
    Chapter *ch = data;
    g_free (ch->label);
    ch->label = NULL;
}

static void
vlx_seek_bar_finalize (GObject *obj)
{
    VlxSeekBar *self = VLX_SEEK_BAR (obj);
    g_array_unref (self->chapters);
    g_free (self->uri);
    g_clear_object (&self->hover_texture);
    g_clear_object (&self->thumb_cache);
    g_clear_object (&self->pill_layout);
    G_OBJECT_CLASS (vlx_seek_bar_parent_class)->finalize (obj);
}

static GtkSizeRequestMode
vlx_seek_bar_get_request_mode (GtkWidget *widget)
{
    (void) widget;
    return GTK_SIZE_REQUEST_CONSTANT_SIZE;
}

static void
vlx_seek_bar_measure (GtkWidget *widget,
                      GtkOrientation orientation,
                      int for_size,
                      int *minimum, int *natural,
                      int *min_baseline, int *nat_baseline)
{
    (void) widget; (void) for_size;
    (void) min_baseline; (void) nat_baseline;
    if (orientation == GTK_ORIENTATION_VERTICAL) {
        *minimum = *natural = (int) WIDGET_HEIGHT;
    } else {
        *minimum = 64;
        *natural = 400;
    }
}

static void
vlx_seek_bar_class_init (VlxSeekBarClass *klass)
{
    GObjectClass   *obj   = G_OBJECT_CLASS (klass);
    GtkWidgetClass *wklass = GTK_WIDGET_CLASS (klass);

    obj->finalize          = vlx_seek_bar_finalize;
    wklass->snapshot       = vlx_seek_bar_snapshot;
    wklass->get_request_mode = vlx_seek_bar_get_request_mode;
    wklass->measure        = vlx_seek_bar_measure;

    seek_signals[SIGNAL_SEEK_REQUESTED] =
        g_signal_new ("seek-requested",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_LAST,
                      0, NULL, NULL, NULL,
                      G_TYPE_NONE, 1, G_TYPE_DOUBLE);
}

static void
vlx_seek_bar_init (VlxSeekBar *self)
{
    self->hover_x  = -1.0;
    self->thumb_r  = THUMB_RADIUS;
    self->last_requested_us = -1;
    self->ab_a     = -1.0;
    self->ab_b     = -1.0;
    self->ab_active = FALSE;
    self->chapters = g_array_new (FALSE, TRUE, sizeof (Chapter));
    g_array_set_clear_func (self->chapters, chapter_clear);
    
    self->thumb_cache = g_object_ref (vlx_thumbnail_cache_get_default ());

    self->pill_layout = gtk_widget_create_pango_layout (GTK_WIDGET (self), "");
    PangoFontDescription *desc = pango_font_description_from_string ("Sans 11.5");
    pango_layout_set_font_description (self->pill_layout, desc);
    pango_font_description_free (desc);

    gtk_widget_set_cursor_from_name (GTK_WIDGET (self), "pointer");

    /* Drag gesture */
    GtkGesture *drag = gtk_gesture_drag_new ();
    gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (drag), 1);
    g_signal_connect (drag, "drag-begin",  G_CALLBACK (on_drag_begin),  self);
    g_signal_connect (drag, "drag-update", G_CALLBACK (on_drag_update), self);
    g_signal_connect (drag, "drag-end",    G_CALLBACK (on_drag_end),    self);
    gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (drag));

    /* Motion controller */
    GtkEventController *motion = gtk_event_controller_motion_new ();
    g_signal_connect (motion, "motion", G_CALLBACK (on_motion), self);
    g_signal_connect (motion, "leave",  G_CALLBACK (on_leave),  self);
    gtk_widget_add_controller (GTK_WIDGET (self), motion);
}

/* ── Public API ──────────────────────────────────────────────────────── */
VlxSeekBar *
vlx_seek_bar_new (void)
{
    return g_object_new (VLX_TYPE_SEEK_BAR, NULL);
}

void vlx_seek_bar_set_position (VlxSeekBar *self, gdouble fraction)
{
    g_return_if_fail (VLX_IS_SEEK_BAR (self));
    if (self->dragging) return;
    self->position = CLAMP (fraction, 0.0, 1.0);
    gtk_widget_queue_draw (GTK_WIDGET (self));
}

void vlx_seek_bar_set_buffered (VlxSeekBar *self, gdouble fraction)
{
    g_return_if_fail (VLX_IS_SEEK_BAR (self));
    self->buffered = CLAMP (fraction, 0.0, 1.0);
    gtk_widget_queue_draw (GTK_WIDGET (self));
}

void vlx_seek_bar_set_duration (VlxSeekBar *self, gint64 duration_us)
{
    g_return_if_fail (VLX_IS_SEEK_BAR (self));
    self->duration_us = duration_us;
}

void vlx_seek_bar_set_uri (VlxSeekBar *self, const gchar *uri)
{
    g_return_if_fail (VLX_IS_SEEK_BAR (self));
    g_free (self->uri);
    self->uri = g_strdup (uri);
    g_clear_object (&self->hover_texture);
    self->last_requested_us = -1;
}

void
vlx_seek_bar_add_chapter (VlxSeekBar *self,
                          gdouble     fraction,
                          const gchar *label)
{
    g_return_if_fail (VLX_IS_SEEK_BAR (self));
    Chapter ch = { .fraction = fraction, .label = g_strdup (label) };
    g_array_append_val (self->chapters, ch);
    gtk_widget_queue_draw (GTK_WIDGET (self));
}

void vlx_seek_bar_clear_chapters (VlxSeekBar *self)
{
    g_return_if_fail (VLX_IS_SEEK_BAR (self));
    g_array_set_size (self->chapters, 0);
    gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
vlx_seek_bar_set_ab_markers (VlxSeekBar *self,
                             gdouble     a_frac,
                             gdouble     b_frac,
                             gboolean    active)
{
    g_return_if_fail (VLX_IS_SEEK_BAR (self));
    self->ab_a      = a_frac;
    self->ab_b      = b_frac;
    self->ab_active = active;
    gtk_widget_queue_draw (GTK_WIDGET (self));
}
