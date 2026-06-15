/* vlx_controls_overlay.c — Auto-hiding playback controls bar
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Layout (GtkBox, horizontal, pinned to bottom of overlay):
 *   [Play/Pause] [Prev] [Next] [SeekBar────────────────] [Time] [Vol] [Fullscreen]
 *
 * Auto-hides after 2 s of inactivity via a GSource timeout.
 * Visibility is animated with AdwAnimation (opacity + slide).
 */

#include "ui/controls_overlay.h"
#include "ui/seek_bar.h"
#include "core/event_bus.h"
#include "utils/log.h"

#include <math.h>

#define HIDE_DELAY_MS 2500

struct _VlxControlsOverlay {
    GtkWidget   parent_instance;

    VlxPlayer  *player;
    VlxEventBus *bus;

    /* Child widgets */
    GtkWidget  *bar;            /* GtkBox — the visible bar */
    GtkWidget  *play_btn;
    GtkWidget  *prev_btn;
    GtkWidget  *next_btn;
    GtkWidget  *skip_prev;
    GtkWidget  *skip_next;
    VlxSeekBar *seek_bar;
    GtkWidget  *time_label;
    GtkWidget  *tracks_btn;
    GtkWidget  *tracks_popover;
    GtkWidget  *tracks_listbox;
    GtkWidget  *vol_btn;
    GtkWidget  *fs_btn;

    /* Auto-hide */
    guint       hide_source_id;
    gboolean    visible;

    /* AdwTimedAnimation for fade */
    AdwAnimation *fade_anim;

    /* Seek ripple state */
    gboolean    ripple_active;
    gboolean    ripple_forward;   /* TRUE = +10s, FALSE = -10s */
    gdouble     ripple_progress;  /* 0.0 → 1.0 */
    guint       ripple_tick_id;   /* gtk_widget_add_tick_callback handle */
    gint64      ripple_start_us;  /* GdkFrameClock timestamp at start */
};

G_DEFINE_TYPE (VlxControlsOverlay, vlx_controls_overlay, GTK_TYPE_WIDGET)

/* ── Time formatting ──────────────────────────────────────────────────── */
static gchar *
fmt_time (gint64 us)
{
    gint64 secs = us / G_USEC_PER_SEC;
    gint h = (gint) (secs / 3600);
    gint m = (gint) ((secs % 3600) / 60);
    gint s = (gint) (secs % 60);
    if (h > 0) return g_strdup_printf ("%d:%02d:%02d", h, m, s);
    return g_strdup_printf ("%d:%02d", m, s);
}

/* ── Auto-hide timer ─────────────────────────────────────────────────── */
static gboolean
hide_timeout_cb (gpointer data)
{
    VlxControlsOverlay *self = VLX_CONTROLS_OVERLAY (data);
    self->hide_source_id = 0;

    if (self->fade_anim) {
        adw_animation_play (self->fade_anim);
    } else {
        gtk_widget_set_opacity (self->bar, 0.0);
        gtk_widget_set_visible (self->bar, FALSE);
    }
    self->visible = FALSE;
    return G_SOURCE_REMOVE;
}

static void
reset_hide_timer (VlxControlsOverlay *self)
{
    if (self->hide_source_id)
        g_source_remove (self->hide_source_id);
    self->hide_source_id = g_timeout_add (HIDE_DELAY_MS, hide_timeout_cb, self);
}

/* ── Seek ripple animation ────────────────────────────────────────────── */
#define RIPPLE_DURATION_US  600000   /* 600 ms in microseconds */

static gboolean
ripple_tick_cb (GtkWidget     *widget,
                GdkFrameClock *clock,
                gpointer       user_data)
{
    (void) user_data;
    VlxControlsOverlay *self = VLX_CONTROLS_OVERLAY (widget);

    gint64 now = gdk_frame_clock_get_frame_time (clock);
    gint64 elapsed = now - self->ripple_start_us;
    self->ripple_progress = (gdouble) elapsed / RIPPLE_DURATION_US;

    if (self->ripple_progress >= 1.0) {
        self->ripple_progress = 1.0;
        self->ripple_active   = FALSE;
        self->ripple_tick_id  = 0;
        gtk_widget_queue_draw (widget);
        return G_SOURCE_REMOVE;
    }
    gtk_widget_queue_draw (widget);
    return G_SOURCE_CONTINUE;
}

static void
vlx_controls_overlay_trigger_ripple (VlxControlsOverlay *self, gboolean forward)
{
    /* Remove any in-progress ripple tick */
    if (self->ripple_tick_id) {
        gtk_widget_remove_tick_callback (GTK_WIDGET (self), self->ripple_tick_id);
        self->ripple_tick_id = 0;
    }
    self->ripple_active   = TRUE;
    self->ripple_forward  = forward;
    self->ripple_progress = 0.0;

    /* Capture start time from the frame clock */
    GdkFrameClock *clock = gtk_widget_get_frame_clock (GTK_WIDGET (self));
    self->ripple_start_us = clock
        ? gdk_frame_clock_get_frame_time (clock)
        : g_get_monotonic_time ();

    self->ripple_tick_id = gtk_widget_add_tick_callback (
        GTK_WIDGET (self), ripple_tick_cb, NULL, NULL);
    gtk_widget_queue_draw (GTK_WIDGET (self));
}

/* Public entry point: keyboard shortcuts call this */
void
vlx_controls_overlay_seek_ripple (VlxControlsOverlay *self, gboolean forward)
{
    g_return_if_fail (VLX_IS_CONTROLS_OVERLAY (self));
    vlx_controls_overlay_trigger_ripple (self, forward);
}

static void
vlx_controls_overlay_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
    VlxControlsOverlay *self = VLX_CONTROLS_OVERLAY (widget);

    /* Snapshot children first */
    GtkWidgetClass *parent_class =
        GTK_WIDGET_CLASS (vlx_controls_overlay_parent_class);
    if (parent_class->snapshot)
        parent_class->snapshot (widget, snapshot);

    if (!self->ripple_active) return;

    gdouble w   = gtk_widget_get_width (widget);
    gdouble h   = gtk_widget_get_height (widget);
    gdouble t   = self->ripple_progress;
    /* ease-out cubic: fast start, slow end */
    gdouble ease = 1.0 - (1.0 - t) * (1.0 - t) * (1.0 - t);
    gdouble alpha = (t < 0.5) ? 1.0 : 1.0 - (t - 0.5) * 2.0;

    /* Horizontal anchor: left quarter for rewind, right quarter for forward */
    gdouble cx = self->ripple_forward ? w * 0.75 : w * 0.25;
    gdouble cy = h * 0.5;

    graphene_rect_t full = GRAPHENE_RECT_INIT (0, 0, w, h);
    cairo_t *cr = gtk_snapshot_append_cairo (snapshot, &full);

    /* Draw 3 expanding concentric arcs */
    for (int ring = 0; ring < 3; ring++) {
        gdouble ring_t = CLAMP (ease - ring * 0.12, 0.0, 1.0);
        gdouble radius = ring_t * MIN (w, h) * 0.22;
        gdouble ring_alpha = alpha * (1.0 - ring * 0.28);
        if (ring_alpha <= 0.0 || radius <= 0.0) continue;

        /* Semi-circular arc facing the seek direction */
        double start_a = self->ripple_forward ? -G_PI / 2.0 : G_PI / 2.0;
        double sweep   = G_PI;
        cairo_set_source_rgba (cr, 1.0, 1.0, 1.0, ring_alpha * 0.55);
        cairo_set_line_width (cr, 2.5);
        cairo_arc (cr, cx, cy, radius, start_a, start_a + sweep);
        cairo_stroke (cr);
    }

    /* ±10s label */
    cairo_select_font_face (cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                            CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size (cr, 18.0);
    const gchar *label = self->ripple_forward ? "+10s" : "-10s";
    cairo_text_extents_t te;
    cairo_text_extents (cr, label, &te);
    cairo_move_to (cr, cx - te.width / 2.0 - te.x_bearing,
                       cy - te.height / 2.0 - te.y_bearing);
    cairo_set_source_rgba (cr, 1.0, 1.0, 1.0, alpha * 0.9);
    cairo_show_text (cr, label);

    cairo_destroy (cr);
}

/* ── Show controls ───────────────────────────────────────────────────── */
void
vlx_controls_overlay_show_briefly (VlxControlsOverlay *self)
{
    g_return_if_fail (VLX_IS_CONTROLS_OVERLAY (self));

    if (!self->visible) {
        gtk_widget_set_opacity (self->bar, 1.0);
        gtk_widget_set_visible (self->bar, TRUE);
        self->visible = TRUE;
    }
    reset_hide_timer (self);
}

/* ── Event bus callbacks ─────────────────────────────────────────────── */
static void
on_position_updated (VlxEventBus *bus,
                     gint64       pos_us,
                     gint64       dur_us,
                     gpointer     data)
{
    VlxControlsOverlay *self = VLX_CONTROLS_OVERLAY (data);
    (void) bus;

    if (dur_us > 0) {
        vlx_seek_bar_set_position (self->seek_bar, (gdouble) pos_us / dur_us);
        vlx_seek_bar_set_duration (self->seek_bar, dur_us);

        gchar *pos_str = fmt_time (pos_us);
        gchar *dur_str = fmt_time (dur_us);
        gchar *label   = g_strdup_printf ("%s / %s", pos_str, dur_str);
        gtk_label_set_text (GTK_LABEL (self->time_label), label);
        g_free (pos_str); g_free (dur_str); g_free (label);
    }
}

static void
on_buffering (VlxEventBus *bus, gint percent, gpointer data)
{
    VlxControlsOverlay *self = VLX_CONTROLS_OVERLAY (data);
    (void) bus;
    vlx_seek_bar_set_buffered (self->seek_bar, percent / 100.0);
}

static void
on_state_changed (VlxEventBus   *bus,
                  VlxPlayerState state,
                  gpointer       data)
{
    VlxControlsOverlay *self = VLX_CONTROLS_OVERLAY (data);
    (void) bus;

    const gchar *icon = (state == VLX_STATE_PLAYING)
        ? "media-playback-pause-symbolic"
        : "media-playback-start-symbolic";

    gtk_button_set_icon_name (GTK_BUTTON (self->play_btn), icon);
    vlx_seek_bar_set_uri (self->seek_bar, vlx_player_get_uri (self->player));
}

static void
on_track_row_activated (GtkListBox *box, GtkListBoxRow *row, gpointer data)
{
    VlxControlsOverlay *self = VLX_CONTROLS_OVERLAY (data);
    (void) box;
    const gchar *stream_id = g_object_get_data (G_OBJECT (row), "stream-id");
    if (stream_id) {
        vlx_player_select_stream (self->player, stream_id);
    }
    gtk_popover_popdown (GTK_POPOVER (self->tracks_popover));
}

static void
on_stream_collection (VlxEventBus *bus, GstStreamCollection *collection, gpointer data)
{
    VlxControlsOverlay *self = VLX_CONTROLS_OVERLAY (data);
    (void) bus;

    /* Clear existing rows */
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child (self->tracks_listbox)) != NULL) {
        gtk_list_box_remove (GTK_LIST_BOX (self->tracks_listbox), child);
    }

    guint n = gst_stream_collection_get_size (collection);
    for (guint i = 0; i < n; i++) {
        GstStream *stream = gst_stream_collection_get_stream (collection, i);
        GstStreamType type = gst_stream_get_stream_type (stream);
        const gchar *stream_id = gst_stream_get_stream_id (stream);

        const gchar *type_str = "Other";
        if (type & GST_STREAM_TYPE_AUDIO) type_str = "Audio";
        else if (type & GST_STREAM_TYPE_VIDEO) type_str = "Video";
        else if (type & GST_STREAM_TYPE_TEXT) type_str = "Subtitle";

        gchar *label_text = g_strdup_printf ("%s: %s", type_str, stream_id);
        GtkWidget *label = gtk_label_new (label_text);
        gtk_widget_set_margin_top (label, 8);
        gtk_widget_set_margin_bottom (label, 8);
        gtk_widget_set_margin_start (label, 12);
        gtk_widget_set_margin_end (label, 12);
        gtk_widget_set_halign (label, GTK_ALIGN_START);

        GtkWidget *row = gtk_list_box_row_new ();
        gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), label);
        g_object_set_data_full (G_OBJECT (row), "stream-id", g_strdup (stream_id), g_free);

        gtk_list_box_append (GTK_LIST_BOX (self->tracks_listbox), row);

        g_free (label_text);
    }
}

/* ── A-B loop → seek bar markers ──────────────────────────────────────── */
static void
on_ab_loop (VlxEventBus *bus,
            gint64       a_us,
            gint64       b_us,
            gboolean     active,
            gpointer     data)
{
    VlxControlsOverlay *self = VLX_CONTROLS_OVERLAY (data);
    (void) bus;

    gint64 dur = vlx_player_get_duration (self->player);
    if (dur <= 0) return;

    gdouble a_frac = (a_us >= 0) ? (gdouble) a_us / dur : -1.0;
    gdouble b_frac = (b_us >= 0) ? (gdouble) b_us / dur : -1.0;
    vlx_seek_bar_set_ab_markers (self->seek_bar, a_frac, b_frac, active);
}

/* ── Chapters → seek bar tick marks ───────────────────────────────────── */
static void
on_chapters_updated (VlxEventBus *bus, GVariant *chapters_v, gpointer data)
{
    VlxControlsOverlay *self = VLX_CONTROLS_OVERLAY (data);
    (void) bus;

    vlx_seek_bar_clear_chapters (self->seek_bar);

    gint64 dur = vlx_player_get_duration (self->player);
    if (dur <= 0) return;

    GVariantIter iter;
    gint64 t;
    g_variant_iter_init (&iter, chapters_v);
    guint idx = 0;
    while (g_variant_iter_next (&iter, "x", &t)) {
        gdouble frac = (gdouble) t / dur;
        gchar *label = g_strdup_printf ("Chapter %u", ++idx);
        vlx_seek_bar_add_chapter (self->seek_bar, frac, label);
        g_free (label);
    }
}

/* ── Seek bar callback ───────────────────────────────────────────────── */
static void
on_seek_requested (VlxSeekBar *bar, gdouble fraction, gpointer data)
{
    VlxControlsOverlay *self = VLX_CONTROLS_OVERLAY (data);
    (void) bar;

    gint64 dur = vlx_player_get_duration (self->player);
    gint64 pos = (gint64) (fraction * dur);
    vlx_player_seek (self->player, pos);
}

/* ── Button callbacks ────────────────────────────────────────────────── */
static void on_play_clicked (GtkButton *b, gpointer d)
{ (void) b; vlx_player_toggle (VLX_CONTROLS_OVERLAY (d)->player); }

static void on_prev_clicked (GtkButton *b, gpointer d)
{
    (void) b;
    VlxControlsOverlay *self = VLX_CONTROLS_OVERLAY (d);
    vlx_player_seek_relative (self->player, -10 * G_USEC_PER_SEC);
    vlx_controls_overlay_trigger_ripple (self, FALSE);
}

static void on_next_clicked (GtkButton *b, gpointer d)
{
    (void) b;
    VlxControlsOverlay *self = VLX_CONTROLS_OVERLAY (d);
    vlx_player_seek_relative (self->player, +10 * G_USEC_PER_SEC);
    vlx_controls_overlay_trigger_ripple (self, TRUE);
}

static void on_skip_prev_clicked (GtkButton *b, gpointer d)
{ (void) b; g_signal_emit_by_name (d, "prev-video"); }

static void on_skip_next_clicked (GtkButton *b, gpointer d)
{ (void) b; g_signal_emit_by_name (d, "next-video"); }

static void
on_fs_clicked (GtkButton *b, gpointer data)
{
    (void) b;
    GtkWidget *widget = GTK_WIDGET (data);
    GtkRoot   *root   = gtk_widget_get_root (widget);
    if (!GTK_IS_WINDOW (root)) return;
    GtkWindow *win = GTK_WINDOW (root);
    if (gtk_window_is_fullscreen (win))
        gtk_window_unfullscreen (win);
    else
        gtk_window_fullscreen (win);
}

void
vlx_controls_overlay_set_fullscreen_state (VlxControlsOverlay *self, gboolean is_fullscreen)
{
    g_return_if_fail (VLX_IS_CONTROLS_OVERLAY (self));
    gtk_button_set_icon_name (GTK_BUTTON (self->fs_btn),
        is_fullscreen ? "view-restore-symbolic" : "view-fullscreen-symbolic");
}

/* ── GtkWidget vfuncs ────────────────────────────────────────────────── */
static void
vlx_controls_overlay_measure (GtkWidget      *widget,
                               GtkOrientation  orientation,
                               int             for_size,
                               int *min, int *nat,
                               int *min_bl, int *nat_bl)
{
    VlxControlsOverlay *self = VLX_CONTROLS_OVERLAY (widget);
    gtk_widget_measure (self->bar, orientation, for_size,
                        min, nat, min_bl, nat_bl);
}

static void
vlx_controls_overlay_size_allocate (GtkWidget *widget,
                                    int w, int h, int baseline)
{
    VlxControlsOverlay *self = VLX_CONTROLS_OVERLAY (widget);
    int child_h;
    gtk_widget_measure (self->bar, GTK_ORIENTATION_VERTICAL, w,
                        &child_h, NULL, NULL, NULL);
    /* Pin to bottom */
    GtkAllocation alloc = { 0, h - child_h, w, child_h };
    gtk_widget_size_allocate (self->bar, &alloc, baseline);
}

static void
vlx_controls_overlay_dispose (GObject *obj)
{
    VlxControlsOverlay *self = VLX_CONTROLS_OVERLAY (obj);

    if (self->hide_source_id) {
        g_source_remove (self->hide_source_id);
        self->hide_source_id = 0;
    }
    if (self->ripple_tick_id) {
        gtk_widget_remove_tick_callback (GTK_WIDGET (self), self->ripple_tick_id);
        self->ripple_tick_id = 0;
    }
    g_clear_object (&self->fade_anim);
    g_clear_object (&self->player);

    gtk_widget_unparent (self->bar);

    G_OBJECT_CLASS (vlx_controls_overlay_parent_class)->dispose (obj);
}

static void
vlx_controls_overlay_class_init (VlxControlsOverlayClass *klass)
{
    GObjectClass   *obj   = G_OBJECT_CLASS (klass);
    GtkWidgetClass *wklass = GTK_WIDGET_CLASS (klass);

    obj->dispose               = vlx_controls_overlay_dispose;
    wklass->measure            = vlx_controls_overlay_measure;
    wklass->size_allocate      = vlx_controls_overlay_size_allocate;
    wklass->snapshot           = vlx_controls_overlay_snapshot;

    gtk_widget_class_set_css_name (wklass, "controlsoverlay");
    
    g_signal_new ("next-video", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
    g_signal_new ("prev-video", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
vlx_controls_overlay_init (VlxControlsOverlay *self)
{
    /* ── Build bar widget ─────────────────────────────────────────── */
    self->bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class (self->bar, "controls-bar");

    /* Play/Pause */
    self->play_btn = gtk_button_new_from_icon_name (
        "media-playback-start-symbolic");
    gtk_widget_add_css_class (self->play_btn, "flat");
    gtk_widget_add_css_class (self->play_btn, "circular");
    gtk_box_append (GTK_BOX (self->bar), self->play_btn);

    /* Prev/Next Video and Seek */
    self->skip_prev = gtk_button_new_from_icon_name ("media-skip-backward-symbolic");
    gtk_widget_add_css_class (self->skip_prev, "flat");
    gtk_box_append (GTK_BOX (self->bar), self->skip_prev);
    
    self->prev_btn = gtk_button_new_from_icon_name ("media-seek-backward-symbolic");
    gtk_widget_add_css_class (self->prev_btn, "flat");
    gtk_box_append (GTK_BOX (self->bar), self->prev_btn);

    self->next_btn = gtk_button_new_from_icon_name ("media-seek-forward-symbolic");
    gtk_widget_add_css_class (self->next_btn, "flat");
    gtk_box_append (GTK_BOX (self->bar), self->next_btn);
    
    self->skip_next = gtk_button_new_from_icon_name ("media-skip-forward-symbolic");
    gtk_widget_add_css_class (self->skip_next, "flat");
    gtk_box_append (GTK_BOX (self->bar), self->skip_next);

    /* Seek bar */
    self->seek_bar = vlx_seek_bar_new ();
    gtk_widget_set_hexpand (GTK_WIDGET (self->seek_bar), TRUE);
    gtk_box_append (GTK_BOX (self->bar), GTK_WIDGET (self->seek_bar));

    /* Time label */
    self->time_label = gtk_label_new ("0:00 / 0:00");
    gtk_widget_add_css_class (self->time_label, "caption");
    gtk_box_append (GTK_BOX (self->bar), self->time_label);

    /* Tracks button */
    self->tracks_btn = gtk_menu_button_new ();
    gtk_menu_button_set_icon_name (GTK_MENU_BUTTON (self->tracks_btn), "view-list-symbolic");
    gtk_widget_add_css_class (self->tracks_btn, "flat");
    gtk_box_append (GTK_BOX (self->bar), self->tracks_btn);

    self->tracks_popover = gtk_popover_new ();
    self->tracks_listbox = gtk_list_box_new ();
    gtk_widget_set_size_request (self->tracks_listbox, 200, -1);
    gtk_popover_set_child (GTK_POPOVER (self->tracks_popover), self->tracks_listbox);
    gtk_menu_button_set_popover (GTK_MENU_BUTTON (self->tracks_btn), self->tracks_popover);

    g_signal_connect (self->tracks_listbox, "row-activated", G_CALLBACK (on_track_row_activated), self);

    /* Volume — GtkScale (0.0–1.0) replacing deprecated GtkVolumeButton */
    GtkWidget *vol_icon = gtk_image_new_from_icon_name ("audio-volume-medium-symbolic");
    gtk_box_append (GTK_BOX (self->bar), vol_icon);

    self->vol_btn = gtk_scale_new_with_range (GTK_ORIENTATION_HORIZONTAL,
                                              0.0, 1.0, 0.05);
    gtk_scale_set_draw_value (GTK_SCALE (self->vol_btn), FALSE);
    gtk_widget_set_size_request (self->vol_btn, 80, -1);
    gtk_box_append (GTK_BOX (self->bar), self->vol_btn);

    /* Fullscreen */
    self->fs_btn = gtk_button_new_from_icon_name (
        "view-fullscreen-symbolic");
    gtk_widget_add_css_class (self->fs_btn, "flat");
    gtk_box_append (GTK_BOX (self->bar), self->fs_btn);

    gtk_widget_set_parent (self->bar, GTK_WIDGET (self));

    /* Styling */
    gtk_widget_set_valign (GTK_WIDGET (self), GTK_ALIGN_END);
    gtk_widget_set_hexpand (GTK_WIDGET (self), TRUE);
}

/* ── Public constructor ──────────────────────────────────────────────── */
VlxControlsOverlay *
vlx_controls_overlay_new (VlxPlayer *player)
{
    g_return_val_if_fail (VLX_IS_PLAYER (player), NULL);

    VlxControlsOverlay *self = g_object_new (VLX_TYPE_CONTROLS_OVERLAY, NULL);
    self->player  = g_object_ref (player);
    self->bus     = vlx_event_bus_get_default ();
    self->visible = TRUE;

    /* Wire player callbacks via event bus */
    g_signal_connect_object (self->bus, "position-updated",
                             G_CALLBACK (on_position_updated), self, 0);
    g_signal_connect_object (self->bus, "state-changed",
                             G_CALLBACK (on_state_changed), self, 0);
    g_signal_connect_object (self->bus, "buffering",
                             G_CALLBACK (on_buffering), self, 0);
    g_signal_connect_object (self->bus, "stream-collection",
                             G_CALLBACK (on_stream_collection), self, 0);
    g_signal_connect_object (self->bus, "ab-loop",
                             G_CALLBACK (on_ab_loop), self, 0);
    g_signal_connect_object (self->bus, "chapters-updated",
                             G_CALLBACK (on_chapters_updated), self, 0);

    /* Wire UI button callbacks */
    g_signal_connect (self->play_btn, "clicked", G_CALLBACK (on_play_clicked), self);
    g_signal_connect (self->prev_btn, "clicked", G_CALLBACK (on_prev_clicked), self);
    g_signal_connect (self->next_btn, "clicked", G_CALLBACK (on_next_clicked), self);
    g_signal_connect (self->skip_prev, "clicked", G_CALLBACK (on_skip_prev_clicked), self);
    g_signal_connect (self->skip_next, "clicked", G_CALLBACK (on_skip_next_clicked), self);
    g_signal_connect (self->fs_btn,   "clicked", G_CALLBACK (on_fs_clicked),   self);
    g_signal_connect (self->seek_bar, "seek-requested", G_CALLBACK (on_seek_requested), self);

    /* Volume binding: GtkAdjustment 'value' ↔ player 'volume' */
    GtkAdjustment *adj = gtk_range_get_adjustment (GTK_RANGE (self->vol_btn));
    g_object_bind_property (adj, "value",
                            player, "volume",
                            G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE);

    reset_hide_timer (self);
    return self;
}
