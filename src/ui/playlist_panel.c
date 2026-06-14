/* vlx_playlist_panel.c — Rich sidebar playlist
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Each row shows:
 *   [48×48 thumbnail] | [Filename (ellipsised)]
 *                     | [Duration badge]         [▶ dot if playing]
 *
 * Thumbnails are loaded async via VlxThumbnailCache.
 * The currently-playing row gets an accent highlight + pulsing dot.
 */

#include "ui/playlist_panel.h"
#include "media/thumbnail.h"
#include "media/history.h"
#include "utils/log.h"

#include <gio/gio.h>
#include <gst/pbutils/pbutils.h>

/* ── Row widgets stored per entry ──────────────────────────────────────── */
typedef struct {
    GtkWidget  *row;
    GtkWidget  *thumb_picture;  /* GtkPicture for thumbnail */
    GtkWidget  *name_label;
    GtkWidget  *dur_label;
    GtkWidget  *now_dot;        /* Small coloured circle */
    gchar      *uri;
    guint       index;
    gpointer    panel;          /* VlxPlaylistPanel* without bringing in full struct */
} PlaylistRow;

struct _VlxPlaylistPanel {
    GtkWidget    parent_instance;

    VlxPlaylist *playlist;
    GtkWidget   *box;        /* outer VBox */
    GtkWidget   *list_box;
    GtkWidget   *scroll;
    GtkWidget   *toolbar;

    GPtrArray   *rows;       /* PlaylistRow* */
    GstDiscoverer *discoverer;
};

G_DEFINE_TYPE (VlxPlaylistPanel, vlx_playlist_panel, GTK_TYPE_WIDGET)

/* ── Helpers ────────────────────────────────────────────────────────────── */
static void
playlist_row_free (gpointer p)
{
    PlaylistRow *r = p;
    g_free (r->uri);
    g_free (r);
}

static gchar *
format_duration (gint64 us)
{
    if (us <= 0) return g_strdup ("—");
    gint64 secs = us / G_USEC_PER_SEC;
    gint h = (gint)(secs / 3600);
    gint m = (gint)((secs % 3600) / 60);
    gint s = (gint)(secs % 60);
    if (h > 0)
        return g_strdup_printf ("%d:%02d:%02d", h, m, s);
    return g_strdup_printf ("%d:%02d", m, s);
}

/* ── Thumbnail callback (main thread via thread_pool) ───────────────────── */
typedef struct { VlxPlaylistPanel *panel; guint index; } ThumbCtx;

static void
on_thumb_ready (const gchar *uri, gint64 position_us, GdkTexture *texture, gpointer user_data)
{
    ThumbCtx *ctx = user_data;
    VlxPlaylistPanel *self = ctx->panel;

    if (!texture || !self->rows || ctx->index >= self->rows->len) {
        g_object_unref (ctx->panel);
        g_free (ctx);
        return;
    }

    PlaylistRow *r = g_ptr_array_index (self->rows, ctx->index);
    if (r && g_strcmp0 (r->uri, uri) == 0 && r->thumb_picture) {
        GdkPaintable *p = GDK_PAINTABLE (texture);
        gtk_picture_set_paintable (GTK_PICTURE (r->thumb_picture), p);
    }

    g_object_unref (ctx->panel);
    g_free (ctx);
}

static void
on_duration_discovered (GstDiscoverer *disc, GstDiscovererInfo *info,
                        GError *err, gpointer data)
{
    VlxPlaylistPanel *self = VLX_PLAYLIST_PANEL (data);
    (void) disc;

    if (err || !self->rows) return;
    
    const gchar *uri = gst_discoverer_info_get_uri (info);
    if (!uri) return;

    for (guint i = 0; i < self->rows->len; i++) {
        PlaylistRow *r = g_ptr_array_index (self->rows, i);
        if (r && g_strcmp0 (r->uri, uri) == 0 && r->dur_label) {
            gint64 dur = gst_discoverer_info_get_duration (info);
            gchar *dur_str = format_duration (dur / 1000); /* ns to us */
            gtk_label_set_text (GTK_LABEL (r->dur_label), dur_str);
            g_free (dur_str);
        }
    }
}

/* ── Update row highlight based on current playlist index ───────────────── */
static void
update_highlights (VlxPlaylistPanel *self)
{
    guint current = vlx_playlist_get_index (self->playlist);

    for (guint i = 0; i < self->rows->len; i++) {
        PlaylistRow *r = g_ptr_array_index (self->rows, i);
        if (!r) continue;

        gboolean active = (i == current);

        if (active) {
            gtk_widget_add_css_class    (r->row, "playlist-row-active");
            gtk_widget_set_visible (r->now_dot, TRUE);
        } else {
            gtk_widget_remove_css_class (r->row, "playlist-row-active");
            gtk_widget_set_visible (r->now_dot, FALSE);
        }
    }
}

/* ── Remove button callback ──────────────────────────────────────────────── */
static void
on_remove_row_clicked (GtkButton *btn, gpointer data)
{
    (void) btn;
    PlaylistRow *r = data;
    VlxPlaylistPanel *self = VLX_PLAYLIST_PANEL (r->panel);
    vlx_playlist_remove (self->playlist, r->index);
}

/* ── Row builder ─────────────────────────────────────────────────────────── */
static PlaylistRow *
build_row (VlxPlaylistPanel *self, guint index, const gchar *uri)
{
    PlaylistRow *r = g_new0 (PlaylistRow, 1);
    r->uri   = g_strdup (uri);
    r->index = index;
    r->panel = self;

    /* --- GtkListBoxRow wrapper --- */
    r->row = gtk_list_box_row_new ();
    gtk_widget_add_css_class (r->row, "playlist-row");

    /* Outer horizontal box */
    GtkWidget *hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start  (hbox, 8);
    gtk_widget_set_margin_end    (hbox, 8);
    gtk_widget_set_margin_top    (hbox, 6);
    gtk_widget_set_margin_bottom (hbox, 6);

    /* ── Thumbnail ── */
    r->thumb_picture = gtk_picture_new ();
    gtk_widget_set_size_request (r->thumb_picture, 64, 48);
    gtk_picture_set_content_fit (GTK_PICTURE (r->thumb_picture),
                                 GTK_CONTENT_FIT_COVER);
    gtk_widget_add_css_class (r->thumb_picture, "playlist-thumb");
    gtk_widget_set_overflow (r->thumb_picture, GTK_OVERFLOW_HIDDEN);
    gtk_box_append (GTK_BOX (hbox), r->thumb_picture);

    /* ── Right column: name + duration row ── */
    GtkWidget *vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand (vbox, TRUE);
    gtk_widget_set_valign  (vbox, GTK_ALIGN_CENTER);

    GFile *f    = g_file_new_for_uri (uri);
    gchar *base = g_file_get_basename (f);
    g_object_unref (f);

    /* Strip extension for cleaner display */
    gchar *dot = g_utf8_strrchr (base, -1, '.');
    if (dot && dot != base) *dot = '\0';

    r->name_label = gtk_label_new (base);
    g_free (base);
    gtk_label_set_ellipsize (GTK_LABEL (r->name_label), PANGO_ELLIPSIZE_END);
    gtk_label_set_xalign    (GTK_LABEL (r->name_label), 0.0f);
    gtk_label_set_lines     (GTK_LABEL (r->name_label), 1);
    gtk_widget_add_css_class (r->name_label, "playlist-name");
    gtk_box_append (GTK_BOX (vbox), r->name_label);

    /* Duration + now-playing dot in a sub-row */
    GtkWidget *meta_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);

    r->dur_label = gtk_label_new ("—");
    gtk_label_set_xalign (GTK_LABEL (r->dur_label), 0.0f);
    gtk_widget_add_css_class (r->dur_label, "playlist-duration");
    gtk_widget_add_css_class (r->dur_label, "dim-label");
    gtk_widget_add_css_class (r->dur_label, "caption");
    gtk_widget_set_hexpand   (r->dur_label, TRUE);
    gtk_box_append (GTK_BOX (meta_row), r->dur_label);

    /* Now-playing dot — a small colored square styled via CSS */
    r->now_dot = gtk_label_new ("●");
    gtk_widget_add_css_class (r->now_dot, "playlist-now-dot");
    gtk_widget_set_visible   (r->now_dot, FALSE);
    gtk_box_append (GTK_BOX (meta_row), r->now_dot);

    gtk_box_append (GTK_BOX (vbox), meta_row);
    gtk_box_append (GTK_BOX (hbox), vbox);

    /* ── Remove button ── */
    GtkWidget *remove_btn = gtk_button_new_from_icon_name ("user-trash-symbolic");
    gtk_widget_add_css_class (remove_btn, "flat");
    gtk_widget_add_css_class (remove_btn, "circular");
    gtk_widget_set_valign (remove_btn, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text (remove_btn, "Remove from Queue");
    g_signal_connect (remove_btn, "clicked", G_CALLBACK (on_remove_row_clicked), r);
    gtk_box_append (GTK_BOX (hbox), remove_btn);

    gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (r->row), hbox);

    /* ── Async thumbnail request ── */
    ThumbCtx *ctx = g_new0 (ThumbCtx, 1);
    ctx->panel = g_object_ref (self);
    ctx->index = index;
    vlx_thumbnail_cache_request (vlx_thumbnail_cache_get_default (),
                                  uri, 0, on_thumb_ready, ctx);

    /* ── Duration via GstDiscoverer (async via GIO) ── */
    if (self->discoverer) {
        gst_discoverer_discover_uri_async (self->discoverer, uri);
    }

    return r;
}

/* ── Rebuild list ───────────────────────────────────────────────────────── */
static void
rebuild_list (VlxPlaylistPanel *self)
{
    /* Detach all rows */
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child (self->list_box)) != NULL)
        gtk_list_box_remove (GTK_LIST_BOX (self->list_box), child);

    /* Free old row descriptors */
    g_ptr_array_set_size (self->rows, 0);

    guint n = vlx_playlist_get_length (self->playlist);
    for (guint i = 0; i < n; i++) {
        const gchar *uri = vlx_playlist_get_uri_at (self->playlist, i);
        PlaylistRow *r   = build_row (self, i, uri);
        g_ptr_array_add (self->rows, r);
        gtk_list_box_append (GTK_LIST_BOX (self->list_box), r->row);
    }

    update_highlights (self);
}

/* ── Signal handlers ────────────────────────────────────────────────────── */
static void
on_playlist_changed (VlxPlaylist *playlist, gpointer data)
{
    (void) playlist;
    rebuild_list (VLX_PLAYLIST_PANEL (data));
}

static void
on_row_activated (GtkListBox    *list,
                  GtkListBoxRow *row,
                  gpointer       data)
{
    VlxPlaylistPanel *self = VLX_PLAYLIST_PANEL (data);
    (void) list;

    gint idx = gtk_list_box_row_get_index (row);
    if (vlx_playlist_set_index (self->playlist, (guint) idx)) {
        const gchar *uri = vlx_playlist_get_current (self->playlist);
        VLX_LOG_INFO (VLX_LOG_DOMAIN_UI,
                      "Playlist activated index %d: %s", idx, uri);
        g_signal_emit_by_name (self, "item-activated", uri);
        update_highlights (self);
    }
}

static void on_clear_clicked   (GtkButton *b, gpointer d)
{ (void) b; vlx_playlist_clear (VLX_PLAYLIST_PANEL (d)->playlist); }

static void on_shuffle_toggled (GtkToggleButton *b, gpointer d)
{
    VlxPlaylistPanel *self = VLX_PLAYLIST_PANEL (d);
    vlx_playlist_set_shuffle (self->playlist,
                              gtk_toggle_button_get_active (b));
}

/* ── GtkWidget vfuncs ───────────────────────────────────────────────────── */
static void
vlx_playlist_panel_measure (GtkWidget      *widget,
                             GtkOrientation  orientation,
                             int             for_size,
                             int *min, int *nat,
                             int *min_bl, int *nat_bl)
{
    VlxPlaylistPanel *self = VLX_PLAYLIST_PANEL (widget);
    gtk_widget_measure (self->box, orientation, for_size,
                        min, nat, min_bl, nat_bl);
}

static void
vlx_playlist_panel_size_allocate (GtkWidget *widget,
                                  int w, int h, int baseline)
{
    VlxPlaylistPanel *self = VLX_PLAYLIST_PANEL (widget);
    GtkAllocation alloc = { 0, 0, w, h };
    gtk_widget_size_allocate (self->box, &alloc, baseline);
}

static void
vlx_playlist_panel_dispose (GObject *obj)
{
    VlxPlaylistPanel *self = VLX_PLAYLIST_PANEL (obj);
    if (self->discoverer) {
        gst_discoverer_stop (self->discoverer);
        g_clear_object (&self->discoverer);
    }
    g_clear_object  (&self->playlist);
    if (self->rows) {
        g_ptr_array_free (self->rows, TRUE);
        self->rows = NULL;
    }
    gtk_widget_unparent (self->box);
    G_OBJECT_CLASS (vlx_playlist_panel_parent_class)->dispose (obj);
}

/* ── Signals ─────────────────────────────────────────────────────────────── */
enum { SIGNAL_ITEM_ACTIVATED, N_SIGNALS };
static guint panel_signals[N_SIGNALS];

static void
vlx_playlist_panel_class_init (VlxPlaylistPanelClass *klass)
{
    GObjectClass   *obj   = G_OBJECT_CLASS (klass);
    GtkWidgetClass *wklass = GTK_WIDGET_CLASS (klass);

    obj->dispose               = vlx_playlist_panel_dispose;
    wklass->measure            = vlx_playlist_panel_measure;
    wklass->size_allocate      = vlx_playlist_panel_size_allocate;

    panel_signals[SIGNAL_ITEM_ACTIVATED] =
        g_signal_new ("item-activated",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_LAST,
                      0, NULL, NULL, NULL,
                      G_TYPE_NONE, 1, G_TYPE_STRING);

    /* CSS for the row styles */
    GtkCssProvider *css = gtk_css_provider_new ();
    gtk_css_provider_load_from_string (css,
        ".playlist-row { padding: 4px; border-radius: 8px; transition: background 0.2s, box-shadow 0.2s; }"
        ".playlist-row:hover { background: alpha(currentColor, 0.06); }"
        ".playlist-row-active { background: alpha(@accent_color, 0.12); box-shadow: inset 3px 0 0 @accent_color; }"
        ".playlist-row-active .playlist-name { font-weight: bold; color: @accent_color; }"
        ".playlist-now-dot { color: @accent_color; font-size: 10px; }"
        ".playlist-thumb { border-radius: 6px; background: alpha(white,0.05); }"
        ".playlist-name { font-size: 11pt; font-weight: 600; }"
        ".playlist-duration { font-size: 9pt; opacity: 0.6; }"
        ".queue-title { font-weight: 800; font-size: 20pt; letter-spacing: -0.5px; }"
        ".history-section-title { font-weight: 800; font-size: 10pt; text-transform: uppercase; letter-spacing: 1.5px; opacity: 0.5; }"
        ".history-icon { background: alpha(@accent_color, 0.15); color: @accent_color; border-radius: 50%; padding: 8px; margin-right: 4px; }"
        ".history-progress trough { min-height: 4px; border-radius: 2px; background: rgba(255, 255, 255, 0.1); }"
        ".history-progress progress { min-height: 4px; border-radius: 2px; background: @accent_color; }"
    );
    gtk_style_context_add_provider_for_display (
        gdk_display_get_default (),
        GTK_STYLE_PROVIDER (css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref (css);
}

static void
vlx_playlist_panel_init (VlxPlaylistPanel *self)
{
    self->rows = g_ptr_array_new_with_free_func (playlist_row_free);

    GError *err = NULL;
    self->discoverer = gst_discoverer_new (5 * GST_SECOND, &err);
    if (self->discoverer) {
        g_signal_connect (self->discoverer, "discovered", G_CALLBACK (on_duration_discovered), self);
        gst_discoverer_start (self->discoverer);
    } else {
        VLX_LOG_ERROR (VLX_LOG_DOMAIN_UI, "Failed to create discoverer: %s", err->message);
        g_clear_error (&err);
    }

    /* Outer box */
    self->box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_parent (self->box, GTK_WIDGET (self));

    /* ── Toolbar ── */
    self->toolbar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_margin_start  (self->toolbar, 8);
    gtk_widget_set_margin_end    (self->toolbar, 8);
    gtk_widget_set_margin_top    (self->toolbar, 8);
    gtk_widget_set_margin_bottom (self->toolbar, 4);

    /* Title label */
    GtkWidget *title = gtk_label_new ("Queue");
    gtk_widget_add_css_class (title, "queue-title");
    gtk_widget_set_hexpand (title, TRUE);
    gtk_label_set_xalign   (GTK_LABEL (title), 0.0f);
    gtk_box_append (GTK_BOX (self->toolbar), title);

    GtkWidget *clear_btn = gtk_button_new_from_icon_name (
        "edit-clear-all-symbolic");
    gtk_widget_add_css_class (clear_btn, "flat");
    gtk_widget_set_tooltip_text (clear_btn, "Clear queue");
    gtk_box_append (GTK_BOX (self->toolbar), clear_btn);

    GtkWidget *shuffle_btn = gtk_toggle_button_new ();
    gtk_button_set_icon_name (GTK_BUTTON (shuffle_btn),
                              "media-playlist-shuffle-symbolic");
    gtk_widget_add_css_class (shuffle_btn, "flat");
    gtk_widget_set_tooltip_text (shuffle_btn, "Shuffle");
    gtk_box_append (GTK_BOX (self->toolbar), shuffle_btn);

    GtkWidget *sep = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
    gtk_box_append (GTK_BOX (self->box), self->toolbar);
    gtk_box_append (GTK_BOX (self->box), sep);

    /* ── Scrolled list ── */
    self->scroll = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (self->scroll),
                                    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand (self->scroll, TRUE);

    self->list_box = gtk_list_box_new ();
    gtk_list_box_set_selection_mode (GTK_LIST_BOX (self->list_box),
                                     GTK_SELECTION_SINGLE);
    gtk_list_box_set_activate_on_single_click (GTK_LIST_BOX (self->list_box),
                                               FALSE);
    gtk_widget_add_css_class (self->list_box, "playlist-list");
    gtk_list_box_set_show_separators (GTK_LIST_BOX (self->list_box), FALSE);

    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (self->scroll),
                                   self->list_box);
    gtk_box_append (GTK_BOX (self->box), self->scroll);

    /* Signals */
    g_signal_connect (clear_btn,      "clicked", G_CALLBACK (on_clear_clicked),   self);
    g_signal_connect (shuffle_btn,    "toggled", G_CALLBACK (on_shuffle_toggled), self);
    g_signal_connect (self->list_box, "row-activated",
                      G_CALLBACK (on_row_activated), self);
}

/* ── Public constructor ─────────────────────────────────────────────────── */
VlxPlaylistPanel *
vlx_playlist_panel_new (VlxPlaylist *playlist)
{
    g_return_val_if_fail (VLX_IS_PLAYLIST (playlist), NULL);

    VlxPlaylistPanel *self = g_object_new (VLX_TYPE_PLAYLIST_PANEL, NULL);
    self->playlist = g_object_ref (playlist);

    g_signal_connect (playlist, "changed",
                      G_CALLBACK (on_playlist_changed), self);

    return self;
}

/* ── Public API: mark current item as playing ───────────────────────────── */
void
vlx_playlist_panel_set_playing (VlxPlaylistPanel *self)
{
    g_return_if_fail (VLX_IS_PLAYLIST_PANEL (self));
    update_highlights (self);
}
