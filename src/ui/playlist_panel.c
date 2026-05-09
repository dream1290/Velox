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

/* ── Row widgets stored per entry ──────────────────────────────────────── */
typedef struct {
    GtkWidget  *row;
    GtkWidget  *thumb_picture;  /* GtkPicture for thumbnail */
    GtkWidget  *name_label;
    GtkWidget  *dur_label;
    GtkWidget  *now_dot;        /* Small coloured circle */
    gchar      *uri;
    guint       index;
} PlaylistRow;

struct _VlxPlaylistPanel {
    GtkWidget    parent_instance;

    VlxPlaylist *playlist;
    GtkWidget   *box;        /* outer VBox */
    GtkWidget   *list_box;
    GtkWidget   *scroll;
    GtkWidget   *toolbar;

    /* Continue Watching (history) */
    GtkWidget   *history_section;  /* outer VBox wrapper */
    GtkWidget   *history_list;     /* GtkListBox */

    GPtrArray   *rows;       /* PlaylistRow* */
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
on_thumb_ready (const gchar *uri, GdkTexture *texture, gpointer user_data)
{
    ThumbCtx *ctx = user_data;
    VlxPlaylistPanel *self = ctx->panel;

    if (!texture || ctx->index >= self->rows->len) {
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

/* ── Row builder ─────────────────────────────────────────────────────────── */
static PlaylistRow *
build_row (VlxPlaylistPanel *self, guint index, const gchar *uri)
{
    PlaylistRow *r = g_new0 (PlaylistRow, 1);
    r->uri   = g_strdup (uri);
    r->index = index;

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

    gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (r->row), hbox);

    /* ── Async thumbnail request ── */
    ThumbCtx *ctx = g_new0 (ThumbCtx, 1);
    ctx->panel = g_object_ref (self);
    ctx->index = index;
    vlx_thumbnail_cache_request (vlx_thumbnail_cache_get_default (),
                                  uri, 0, on_thumb_ready, ctx);

    /* ── Duration via GstDiscoverer (async via GIO) ── */
    /* For simplicity, set duration label after media is opened.
       A future improvement could use GstDiscoverer here. */
    (void) format_duration;  /* suppress unused-function warning for now */

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
    g_clear_object  (&self->playlist);
    g_ptr_array_free (self->rows, TRUE);
    self->rows = NULL;
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
        ".playlist-row { border-radius: 6px; }"
        ".playlist-row-active { background: alpha(@accent_color, 0.18); }"
        ".playlist-row-active .playlist-name { font-weight: bold; }"
        ".playlist-now-dot { color: @accent_color; font-size: 10px; }"
        ".playlist-thumb { border-radius: 4px; background: alpha(white,0.08); }"
        ".playlist-name { font-size: 13px; }"
        ".playlist-duration { font-size: 11px; }"
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
    gtk_widget_add_css_class (title, "title-4");
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

    /* ── Continue Watching section ── */
    self->history_section = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

    GtkWidget *hist_label = gtk_label_new ("Continue Watching");
    gtk_widget_add_css_class (hist_label, "title-4");
    gtk_widget_set_margin_start  (hist_label, 8);
    gtk_widget_set_margin_top    (hist_label, 12);
    gtk_widget_set_margin_bottom (hist_label, 4);
    gtk_label_set_xalign (GTK_LABEL (hist_label), 0.0f);
    gtk_box_append (GTK_BOX (self->history_section), hist_label);

    self->history_list = gtk_list_box_new ();
    gtk_list_box_set_selection_mode (GTK_LIST_BOX (self->history_list),
                                     GTK_SELECTION_NONE);
    gtk_list_box_set_activate_on_single_click (
        GTK_LIST_BOX (self->history_list), TRUE);
    gtk_widget_add_css_class (self->history_list, "playlist-list");
    gtk_box_append (GTK_BOX (self->history_section), self->history_list);

    GtkWidget *hist_sep = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
    gtk_box_append (GTK_BOX (self->history_section), hist_sep);

    gtk_box_append (GTK_BOX (self->box), self->history_section);

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

/* ── History row activated → emit item-activated ────────────────────────── */
static void
on_history_row_activated (GtkListBox    *list,
                          GtkListBoxRow *row,
                          gpointer       data)
{
    VlxPlaylistPanel *self = VLX_PLAYLIST_PANEL (data);
    (void) list;

    const gchar *uri = g_object_get_data (G_OBJECT (row), "history-uri");
    if (uri) {
        VLX_LOG_INFO (VLX_LOG_DOMAIN_UI, "History: resuming %s", uri);
        g_signal_emit_by_name (self, "item-activated", uri);
    }
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

    /* Wire history list activation */
    g_signal_connect (self->history_list, "row-activated",
                      G_CALLBACK (on_history_row_activated), self);

    /* Populate history on creation */
    vlx_playlist_panel_refresh_history (self);

    return self;
}

/* ── Public API: mark current item as playing ───────────────────────────── */
void
vlx_playlist_panel_set_playing (VlxPlaylistPanel *self)
{
    g_return_if_fail (VLX_IS_PLAYLIST_PANEL (self));
    update_highlights (self);
}

/* ── Public API: refresh the Continue Watching section ───────────────────── */
void
vlx_playlist_panel_refresh_history (VlxPlaylistPanel *self)
{
    g_return_if_fail (VLX_IS_PLAYLIST_PANEL (self));

    /* Clear existing rows */
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child (self->history_list)) != NULL)
        gtk_list_box_remove (GTK_LIST_BOX (self->history_list), child);

    VlxHistory *history = vlx_history_get_default ();
    GPtrArray  *entries = vlx_history_list (history, 8);

    if (entries->len == 0) {
        gtk_widget_set_visible (self->history_section, FALSE);
        g_ptr_array_unref (entries);
        return;
    }
    gtk_widget_set_visible (self->history_section, TRUE);

    for (guint i = 0; i < entries->len; i++) {
        VlxHistoryEntry *e = g_ptr_array_index (entries, i);

        GtkWidget *row = gtk_list_box_row_new ();
        gtk_widget_add_css_class (row, "playlist-row");

        GtkWidget *hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_margin_start  (hbox, 8);
        gtk_widget_set_margin_end    (hbox, 8);
        gtk_widget_set_margin_top    (hbox, 6);
        gtk_widget_set_margin_bottom (hbox, 6);

        /* Icon */
        GtkWidget *icon = gtk_image_new_from_icon_name ("media-playback-start-symbolic");
        gtk_widget_set_valign (icon, GTK_ALIGN_CENTER);
        gtk_box_append (GTK_BOX (hbox), icon);

        /* Info column */
        GtkWidget *vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
        gtk_widget_set_hexpand (vbox, TRUE);

        /* Title: use basename of URI if title is empty */
        const gchar *display = (e->title && e->title[0]) ? e->title : NULL;
        if (!display) {
            GFile *f = g_file_new_for_uri (e->uri);
            display = g_file_get_basename (f);
            g_object_unref (f);
        }
        GtkWidget *name = gtk_label_new (display);
        gtk_label_set_ellipsize (GTK_LABEL (name), PANGO_ELLIPSIZE_END);
        gtk_label_set_xalign (GTK_LABEL (name), 0.0f);
        gtk_label_set_lines (GTK_LABEL (name), 1);
        gtk_widget_add_css_class (name, "playlist-name");
        gtk_box_append (GTK_BOX (vbox), name);

        /* Progress bar */
        gdouble progress = 0.0;
        if (e->duration_us > 0)
            progress = CLAMP ((gdouble)e->last_pos_us / e->duration_us, 0.0, 1.0);

        GtkWidget *prog = gtk_progress_bar_new ();
        gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (prog), progress);
        gtk_widget_set_valign (prog, GTK_ALIGN_CENTER);
        gtk_widget_add_css_class (prog, "osd");
        gtk_box_append (GTK_BOX (vbox), prog);

        gtk_box_append (GTK_BOX (hbox), vbox);

        gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), hbox);

        /* Stash URI for activation */
        g_object_set_data_full (G_OBJECT (row), "history-uri",
                                g_strdup (e->uri), g_free);

        gtk_list_box_append (GTK_LIST_BOX (self->history_list), row);
    }

    g_ptr_array_unref (entries);
}
