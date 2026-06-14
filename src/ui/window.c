/* vlx_window.c — Main application window (programmatic construction)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ui/window.h"
#include "ui/video_widget.h"
#include "ui/controls_overlay.h"
#include "ui/playlist_panel.h"
#include "ui/settings_dialog.h"
#include "ui/subtitle_overlay.h"
#include "ui/pip_window.h"
#include "core/player.h"
#include "core/playlist.h"
#include "core/event_bus.h"
#include "media/history.h"
#include "platform/settings.h"
#include "utils/log.h"
#include <stdarg.h>

struct _VlxWindow {
    AdwApplicationWindow  parent_instance;

    VlxPlayer            *player;
    VlxPlaylist          *playlist;
    VlxEventBus          *bus;

    /* Widgets */
    GtkWidget            *overlay;
    VlxVideoWidget       *video_widget;
    VlxControlsOverlay   *controls;
    VlxSubtitleOverlay   *subtitle_overlay;
    VlxPlaylistPanel     *playlist_panel;

    GtkWidget            *open_btn;
    GtkWidget            *queue_btn;
    GtkWidget            *menu_btn;
    GtkWidget            *hbar;
    AdwToolbarView       *toolbar_view;
    AdwOverlaySplitView  *split;
    AdwToastOverlay      *toast_overlay;

    GtkWidget            *hud_label;
    guint                 hud_timeout_id;
    
    GtkWidget            *spinner;
    GstStreamCollection  *collection;

    /* P3 — Power user */
    VlxPipWindow         *pip_window;
    VlxHistory           *history;

    /* P4 — Video balance (live, clamped) */
    gdouble               brightness;   /* -1.0 .. 1.0, default 0.0 */
    gdouble               contrast;     /*  0.0 .. 2.0, default 1.0 */
    gdouble               saturation;   /*  0.0 .. 2.0, default 1.0 */
    gdouble               hue;          /* -1.0 .. 1.0, default 0.0 */
};

G_DEFINE_TYPE (VlxWindow, vlx_window, ADW_TYPE_APPLICATION_WINDOW)

/* ── Forward declarations ─────────────────────────────────────────────── */
static void vlx_window_show_hud (VlxWindow *self, const gchar *format, ...);
static void vlx_window_show_media_info (VlxWindow *self);
static void vlx_window_show_open_url_dialog (VlxWindow *self);
static void vlx_window_show_cheat_sheet (VlxWindow *self);

static void on_open_file_ready (GObject *, GAsyncResult *, gpointer);

/* ── Unified playlist playback helper ─────────────────────────────────── */
static void
play_playlist_uri (VlxWindow *self, const gchar *uri)
{
    if (!uri) return;
    vlx_player_open (self->player, uri);
    GstElement *sink = vlx_player_get_video_sink (self->player);
    vlx_video_widget_set_sink (self->video_widget, sink);
    vlx_playlist_panel_set_playing (self->playlist_panel);
}

static void
act_open_file (GSimpleAction *action, GVariant *param, gpointer data)
{
    (void) action; (void) param;
    VlxWindow *self = VLX_WINDOW (data);
    GtkFileDialog *dialog = gtk_file_dialog_new ();
    gtk_file_dialog_set_title (dialog, "Open Media");

    GListStore    *filters      = g_list_store_new (GTK_TYPE_FILE_FILTER);
    GtkFileFilter *video_filter = gtk_file_filter_new ();
    gtk_file_filter_set_name (video_filter, "Video & Audio files");
    gtk_file_filter_add_mime_type (video_filter, "video/*");
    gtk_file_filter_add_mime_type (video_filter, "audio/*");
    g_list_store_append (filters, video_filter);
    gtk_file_dialog_set_filters (dialog, G_LIST_MODEL (filters));
    g_object_unref (video_filter);
    g_object_unref (filters);

    gtk_file_dialog_open (dialog, GTK_WINDOW (self), NULL,
                          on_open_file_ready, self);
    g_object_unref (dialog);
}

static void
act_open_url (GSimpleAction *action, GVariant *param, gpointer data)
{ (void) action; (void) param; vlx_window_show_open_url_dialog (VLX_WINDOW (data)); }

static void
act_play_pause (GSimpleAction *action, GVariant *param, gpointer data)
{
    (void) action; (void) param;
    VlxWindow *self = VLX_WINDOW (data);
    vlx_player_toggle (self->player);
    vlx_window_show_hud (self, "Play / Pause");
}

static void
act_stop (GSimpleAction *action, GVariant *param, gpointer data)
{
    (void) action; (void) param;
    VlxWindow *self = VLX_WINDOW (data);
    vlx_player_stop (self->player);
}

static void
act_fullscreen (GSimpleAction *action, GVariant *param, gpointer data)
{
    (void) action; (void) param;
    VlxWindow *self = VLX_WINDOW (data);
    if (gtk_window_is_fullscreen (GTK_WINDOW (self)))
        gtk_window_unfullscreen (GTK_WINDOW (self));
    else
        gtk_window_fullscreen (GTK_WINDOW (self));
}

static void
act_screenshot (GSimpleAction *action, GVariant *param, gpointer data)
{
    (void) action; (void) param;
    VlxWindow *self = VLX_WINDOW (data);
    gchar *dir = g_build_filename (g_get_user_special_dir (G_USER_DIRECTORY_PICTURES), "Velox", NULL);
    g_mkdir_with_parents (dir, 0755);
    gint64 pos = vlx_player_get_position (self->player);
    gchar *name = g_strdup_printf ("frame_%08ld.png", pos / 1000);
    gchar *path = g_build_filename (dir, name, NULL);
    vlx_video_widget_take_screenshot (self->video_widget, path);
    vlx_window_show_hud (self, "\u2709 Screenshot saved");
    gchar *toast_msg = g_strdup_printf ("Saved to %s", path);
    adw_toast_overlay_add_toast (self->toast_overlay, adw_toast_new (toast_msg));
    g_free (toast_msg); g_free (path); g_free (name); g_free (dir);
}

static void
act_pip (GSimpleAction *action, GVariant *param, gpointer data)
{
    (void) action; (void) param;
    VlxWindow *self = VLX_WINDOW (data);
    if (self->pip_window && gtk_widget_get_visible (GTK_WIDGET (self->pip_window))) {
        vlx_pip_window_close_pip (self->pip_window);
        vlx_window_show_hud (self, "PiP closed");
    } else {
        if (!self->pip_window)
            self->pip_window = vlx_pip_window_new (self->video_widget);
        vlx_pip_window_present (self->pip_window, GTK_WINDOW (self));
        vlx_window_show_hud (self, "\u2B1C PiP opened");
    }
}

static void
act_media_info (GSimpleAction *action, GVariant *param, gpointer data)
{ (void) action; (void) param; vlx_window_show_media_info (VLX_WINDOW (data)); }

static void
act_preferences (GSimpleAction *action, GVariant *param, gpointer data)
{
    (void) action; (void) param;
    VlxWindow *self = VLX_WINDOW (data);
    VlxSettingsDialog *dlg = vlx_settings_dialog_new ();
    adw_dialog_present (ADW_DIALOG (dlg), GTK_WIDGET (self));
}

static void
act_toggle_queue (GSimpleAction *action, GVariant *param, gpointer data)
{
    (void) action; (void) param;
    VlxWindow *self = VLX_WINDOW (data);
    gboolean show = !adw_overlay_split_view_get_show_sidebar (self->split);
    adw_overlay_split_view_set_show_sidebar (self->split, show);
}

static void
act_shortcuts (GSimpleAction *action, GVariant *param, gpointer data)
{ (void) action; (void) param; vlx_window_show_cheat_sheet (VLX_WINDOW (data)); }

static void
act_clear_history (GSimpleAction *action, GVariant *param, gpointer data)
{
    (void) action; (void) param;
    VlxWindow *self = VLX_WINDOW (data);
    if (self->history) {
        vlx_history_clear_all (self->history);
        vlx_window_show_hud (self, "History cleared");
        adw_toast_overlay_add_toast (self->toast_overlay,
            adw_toast_new ("Watch history cleared"));
    }
}

static void
act_ab_loop (GSimpleAction *action, GVariant *param, gpointer data)
{
    (void) action; (void) param;
    VlxWindow *self = VLX_WINDOW (data);
    gint64 a, b;
    gboolean active = vlx_player_get_ab_state (self->player, &a, &b);
    if (active) {
        vlx_player_clear_ab (self->player);
        vlx_window_show_hud (self, "A-B Loop cleared");
    } else if (a >= 0) {
        vlx_player_set_ab_b (self->player);
        vlx_player_get_ab_state (self->player, &a, &b);
        vlx_window_show_hud (self, b >= 0 ? "\u21BB A-B Loop active" : "B must be after A");
    } else {
        vlx_player_set_ab_a (self->player);
        vlx_window_show_hud (self, "A set \u2014 press again for B");
    }
}

static void
act_speed_reset (GSimpleAction *action, GVariant *param, gpointer data)
{
    (void) action; (void) param;
    VlxWindow *self = VLX_WINDOW (data);
    vlx_player_set_rate (self->player, 1.0);
    vlx_window_show_hud (self, "Speed: 1.0x");
}

/* ── Register all window actions and build hamburger menu ─────────────── */
static void
vlx_window_setup_actions_and_menu (VlxWindow *self)
{
    static const GActionEntry entries[] = {
        { .name = "open-file",       .activate = act_open_file,      .parameter_type = NULL, .state = NULL, .change_state = NULL },
        { .name = "open-url",        .activate = act_open_url,       .parameter_type = NULL, .state = NULL, .change_state = NULL },
        { .name = "play-pause",      .activate = act_play_pause,     .parameter_type = NULL, .state = NULL, .change_state = NULL },
        { .name = "stop",            .activate = act_stop,           .parameter_type = NULL, .state = NULL, .change_state = NULL },
        { .name = "fullscreen",      .activate = act_fullscreen,     .parameter_type = NULL, .state = NULL, .change_state = NULL },
        { .name = "screenshot",      .activate = act_screenshot,     .parameter_type = NULL, .state = NULL, .change_state = NULL },
        { .name = "pip",             .activate = act_pip,            .parameter_type = NULL, .state = NULL, .change_state = NULL },
        { .name = "media-info",      .activate = act_media_info,     .parameter_type = NULL, .state = NULL, .change_state = NULL },
        { .name = "preferences",     .activate = act_preferences,    .parameter_type = NULL, .state = NULL, .change_state = NULL },
        { .name = "toggle-queue",    .activate = act_toggle_queue,   .parameter_type = NULL, .state = NULL, .change_state = NULL },
        { .name = "shortcuts",       .activate = act_shortcuts,      .parameter_type = NULL, .state = NULL, .change_state = NULL },
        { .name = "clear-history",   .activate = act_clear_history,  .parameter_type = NULL, .state = NULL, .change_state = NULL },
        { .name = "ab-loop",         .activate = act_ab_loop,        .parameter_type = NULL, .state = NULL, .change_state = NULL },
        { .name = "speed-reset",     .activate = act_speed_reset,    .parameter_type = NULL, .state = NULL, .change_state = NULL },
    };
    g_action_map_add_action_entries (G_ACTION_MAP (self), entries,
                                     G_N_ELEMENTS (entries), self);

    /* ── Build GMenu ─────────────────────────────────────────────────── */
    GMenu *menu = g_menu_new ();

    /* — Media section — */
    GMenu *media = g_menu_new ();
    g_menu_append (media, "Open File…",          "win.open-file");
    g_menu_append (media, "Open Network Stream…","win.open-url");
    g_menu_append_section (menu, NULL, G_MENU_MODEL (media));
    g_object_unref (media);

    /* — Playback section — */
    GMenu *playback = g_menu_new ();
    g_menu_append (playback, "Play / Pause",      "win.play-pause");
    g_menu_append (playback, "Stop",              "win.stop");
    g_menu_append (playback, "A-B Loop",          "win.ab-loop");
    g_menu_append (playback, "Reset Speed",       "win.speed-reset");
    g_menu_append_section (menu, "Playback", G_MENU_MODEL (playback));
    g_object_unref (playback);

    /* — View section — */
    GMenu *view = g_menu_new ();
    g_menu_append (view, "Toggle Queue",          "win.toggle-queue");
    g_menu_append (view, "Fullscreen",            "win.fullscreen");
    g_menu_append (view, "Picture-in-Picture",    "win.pip");
    g_menu_append (view, "Screenshot",            "win.screenshot");
    g_menu_append_section (menu, "View", G_MENU_MODEL (view));
    g_object_unref (view);

    /* — Tools section — */
    GMenu *tools = g_menu_new ();
    g_menu_append (tools, "Media Info",           "win.media-info");
    g_menu_append (tools, "Clear Watch History",  "win.clear-history");
    g_menu_append (tools, "Preferences…",         "win.preferences");
    g_menu_append_section (menu, "Tools", G_MENU_MODEL (tools));
    g_object_unref (tools);

    /* — Help section — */
    GMenu *help = g_menu_new ();
    g_menu_append (help, "Keyboard Shortcuts",    "win.shortcuts");
    g_menu_append_section (menu, NULL, G_MENU_MODEL (help));
    g_object_unref (help);

    gtk_menu_button_set_menu_model (GTK_MENU_BUTTON (self->menu_btn),
                                    G_MENU_MODEL (menu));
    g_object_unref (menu);
}

/* ── File-open callback (named — no C lambdas) ────────────────────────── */
static void
on_open_file_ready (GObject *source, GAsyncResult *result, gpointer data)
{
    VlxWindow     *self   = VLX_WINDOW (data);
    GtkFileDialog *dialog = GTK_FILE_DIALOG (source);
    GError        *err    = NULL;

    GFile *file = gtk_file_dialog_open_finish (dialog, result, &err);
    if (!file) {
        if (err && !g_error_matches (err, GTK_DIALOG_ERROR,
                                     GTK_DIALOG_ERROR_DISMISSED))
            VLX_LOG_WARNING (VLX_LOG_DOMAIN_UI,
                             "File open error: %s", err->message);
        g_clear_error (&err);
        return;
    }

    gchar *uri = g_file_get_uri (file);
    vlx_window_open (self, uri);
    g_free (uri);
    g_object_unref (file);
}

static void
on_open_clicked (GtkButton *btn, gpointer data)
{
    VlxWindow *self = VLX_WINDOW (data);
    (void) btn;

    GtkFileDialog *dialog = gtk_file_dialog_new ();
    gtk_file_dialog_set_title (dialog, "Open Media");

    GListStore    *filters      = g_list_store_new (GTK_TYPE_FILE_FILTER);
    GtkFileFilter *video_filter = gtk_file_filter_new ();
    gtk_file_filter_set_name (video_filter, "Video & Audio files");
    gtk_file_filter_add_mime_type (video_filter, "video/*");
    gtk_file_filter_add_mime_type (video_filter, "audio/*");
    g_list_store_append (filters, video_filter);
    gtk_file_dialog_set_filters (dialog, G_LIST_MODEL (filters));
    g_object_unref (video_filter);
    g_object_unref (filters);

    gtk_file_dialog_open (dialog, GTK_WINDOW (self), NULL,
                          on_open_file_ready, self);
    g_object_unref (dialog);
}

/* ── Motion / cursor show ─────────────────────────────────────────────── */
static void
on_motion (GtkEventControllerMotion *ctrl,
           gdouble x, gdouble y,
           gpointer data)
{
    (void) ctrl; (void) x; (void) y;
    vlx_controls_overlay_show_briefly (VLX_WINDOW (data)->controls);
}

/* ── Click-to-play/pause on video area ────────────────────────────────── */
static void
on_video_clicked (GtkGestureClick *gesture,
                  gint n_press,
                  gdouble x, gdouble y,
                  gpointer data)
{
    (void) gesture; (void) x; (void) y;
    VlxWindow *self = VLX_WINDOW (data);

    if (n_press == 1) {
        /* Single click: play/pause */
        vlx_player_toggle (self->player);
        vlx_window_show_hud (self, "Play / Pause");
    } else if (n_press == 2) {
        /* Double click: fullscreen toggle */
        if (gtk_window_is_fullscreen (GTK_WINDOW (self)))
            gtk_window_unfullscreen (GTK_WINDOW (self));
        else
            gtk_window_fullscreen (GTK_WINDOW (self));
    }
    /* Grab focus to the overlay so Space key works after clicking */
    gtk_widget_grab_focus (self->overlay);
}

/* ── Title update from event bus ──────────────────────────────────────── */
static void
on_state_changed (VlxEventBus   *bus,
                  VlxPlayerState state,
                  gpointer       data)
{
    VlxWindow *self = VLX_WINDOW (data);
    (void) bus;

    if (state == VLX_STATE_PLAYING || state == VLX_STATE_PAUSED || state == VLX_STATE_LOADING) {
        if (state == VLX_STATE_LOADING) {
            gtk_widget_set_visible (self->spinner, TRUE);
        } else {
            gtk_widget_set_visible (self->spinner, FALSE);
        }

        const gchar *uri = vlx_player_get_uri (self->player);
        if (uri) {
            GFile *f        = g_file_new_for_uri (uri);
            gchar *basename = g_file_get_basename (f);
            gchar *title    = g_strdup_printf ("%s — Velox", basename);
            gtk_window_set_title (GTK_WINDOW (self), title);
            g_free (title);
            g_free (basename);
            g_object_unref (f);
        }
        if (state == VLX_STATE_PLAYING)
            vlx_playlist_panel_set_playing (self->playlist_panel);
        return;
    }
    gtk_widget_set_visible (self->spinner, FALSE);
    gtk_window_set_title (GTK_WINDOW (self), "Velox");
}

static void
on_playlist_item_activated (VlxPlaylistPanel *panel, const gchar *uri, gpointer data)
{
    (void) panel;
    play_playlist_uri (VLX_WINDOW (data), uri);
}

static void
on_fullscreened (GObject *obj, GParamSpec *pspec, gpointer data)
{
    VlxWindow *self = VLX_WINDOW (obj);
    (void) pspec; (void) data;
    gboolean is_fs = gtk_window_is_fullscreen (GTK_WINDOW (self));
    adw_toolbar_view_set_reveal_top_bars (self->toolbar_view, !is_fs);
    if (is_fs) {
        adw_overlay_split_view_set_show_sidebar (self->split, FALSE);
    }
    vlx_controls_overlay_set_fullscreen_state (self->controls, is_fs);
}

static void
on_eos (VlxEventBus *bus, gpointer data)
{
    (void) bus;
    VlxWindow *self = VLX_WINDOW (data);
    play_playlist_uri (self, vlx_playlist_next (self->playlist));
}

static void
on_stream_collection (VlxEventBus *bus, GstStreamCollection *collection, gpointer data)
{
    VlxWindow *self = VLX_WINDOW (data);
    g_set_object (&self->collection, collection);
}

static void
on_controls_next (VlxControlsOverlay *controls, gpointer data)
{
    (void) controls;
    VlxWindow *self = VLX_WINDOW (data);
    play_playlist_uri (self, vlx_playlist_next (self->playlist));
}

static void
on_controls_prev (VlxControlsOverlay *controls, gpointer data)
{
    (void) controls;
    VlxWindow *self = VLX_WINDOW (data);
    play_playlist_uri (self, vlx_playlist_previous (self->playlist));
}

static gboolean
hide_hud_cb (gpointer data)
{
    VlxWindow *self = VLX_WINDOW (data);
    gtk_widget_set_opacity (self->hud_label, 0.0);
    self->hud_timeout_id = 0;
    return G_SOURCE_REMOVE;
}

static void
vlx_window_show_hud (VlxWindow *self, const gchar *format, ...)
{
    va_list args;
    va_start (args, format);
    gchar *text = g_strdup_vprintf (format, args);
    va_end (args);

    gtk_label_set_text (GTK_LABEL (self->hud_label), text);
    gtk_widget_set_opacity (self->hud_label, 1.0);
    g_free (text);

    if (self->hud_timeout_id)
        g_source_remove (self->hud_timeout_id);
    self->hud_timeout_id = g_timeout_add (1200, hide_hud_cb, self);
}

static void
vlx_window_show_media_info (VlxWindow *self)
{
    AdwDialog *dialog = adw_dialog_new ();
    adw_dialog_set_presentation_mode (dialog, ADW_DIALOG_BOTTOM_SHEET);
    adw_dialog_set_title (dialog, "Media Info");

    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 16);
    gtk_widget_set_margin_start (box, 24);
    gtk_widget_set_margin_end (box, 24);
    gtk_widget_set_margin_top (box, 24);
    gtk_widget_set_margin_bottom (box, 24);

    AdwPreferencesGroup *grp = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
    gtk_box_append (GTK_BOX (box), GTK_WIDGET (grp));

    const gchar *uri = vlx_player_get_uri (self->player);
    if (!uri) uri = "Unknown";

    AdwActionRow *uri_row = ADW_ACTION_ROW (adw_action_row_new ());
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (uri_row), "URI");
    adw_action_row_set_subtitle (uri_row, uri);
    adw_action_row_set_subtitle_lines (uri_row, 0);
    adw_preferences_group_add (grp, GTK_WIDGET (uri_row));

    gchar *size_str = g_strdup ("Unknown");
    gchar *mtime_str = g_strdup ("Unknown");

    if (g_str_has_prefix (uri, "file://")) {
        GFile *f = g_file_new_for_uri (uri);
        GFileInfo *info = g_file_query_info (f, G_FILE_ATTRIBUTE_STANDARD_SIZE "," G_FILE_ATTRIBUTE_TIME_MODIFIED, G_FILE_QUERY_INFO_NONE, NULL, NULL);
        if (info) {
            guint64 size = g_file_info_get_size (info);
            g_free (size_str);
            size_str = g_format_size (size);

            GDateTime *dt = g_file_info_get_modification_date_time (info);
            if (dt) {
                g_free (mtime_str);
                mtime_str = g_date_time_format (dt, "%F %T");
                g_date_time_unref (dt);
            }
            g_object_unref (info);
        }
        g_object_unref (f);
    }

    AdwActionRow *size_row = ADW_ACTION_ROW (adw_action_row_new ());
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (size_row), "Size");
    adw_action_row_set_subtitle (size_row, size_str);
    adw_preferences_group_add (grp, GTK_WIDGET (size_row));

    AdwActionRow *mtime_row = ADW_ACTION_ROW (adw_action_row_new ());
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (mtime_row), "Modified");
    adw_action_row_set_subtitle (mtime_row, mtime_str);
    adw_preferences_group_add (grp, GTK_WIDGET (mtime_row));

    gint64 dur = vlx_player_get_duration (self->player);
    gchar *dur_str = g_strdup_printf ("%" G_GINT64_FORMAT ".%03" G_GINT64_FORMAT " s", dur / G_USEC_PER_SEC, (dur % G_USEC_PER_SEC) / 1000);
    AdwActionRow *dur_row = ADW_ACTION_ROW (adw_action_row_new ());
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (dur_row), "Duration");
    adw_action_row_set_subtitle (dur_row, dur_str);
    adw_preferences_group_add (grp, GTK_WIDGET (dur_row));

    gchar *vid_codec = g_strdup ("Unknown");
    gchar *aud_codec = g_strdup ("Unknown");

    if (self->collection) {
        guint n = gst_stream_collection_get_size (self->collection);
        for (guint i = 0; i < n; i++) {
            GstStream *stream = gst_stream_collection_get_stream (self->collection, i);
            GstStreamType type = gst_stream_get_stream_type (stream);
            GstCaps *caps = gst_stream_get_caps (stream);
            if (caps) {
                gchar *caps_str = gst_caps_to_string (caps);
                if (type & GST_STREAM_TYPE_VIDEO && g_strcmp0(vid_codec, "Unknown") == 0) {
                    g_free (vid_codec);
                    vid_codec = g_strdup (caps_str);
                } else if (type & GST_STREAM_TYPE_AUDIO && g_strcmp0(aud_codec, "Unknown") == 0) {
                    g_free (aud_codec);
                    aud_codec = g_strdup (caps_str);
                }
                g_free (caps_str);
                gst_caps_unref (caps);
            }
        }
    }

    AdwActionRow *vid_row = ADW_ACTION_ROW (adw_action_row_new ());
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (vid_row), "Video Codec");
    adw_action_row_set_subtitle (vid_row, vid_codec);
    adw_action_row_set_subtitle_lines (vid_row, 0);
    adw_preferences_group_add (grp, GTK_WIDGET (vid_row));

    AdwActionRow *aud_row = ADW_ACTION_ROW (adw_action_row_new ());
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (aud_row), "Audio Codec");
    adw_action_row_set_subtitle (aud_row, aud_codec);
    adw_action_row_set_subtitle_lines (aud_row, 0);
    adw_preferences_group_add (grp, GTK_WIDGET (aud_row));

    g_free (size_str);
    g_free (mtime_str);
    g_free (dur_str);
    g_free (vid_codec);
    g_free (aud_codec);

    adw_dialog_set_child (dialog, box);
    adw_dialog_present (dialog, GTK_WIDGET (self));
}

static void
on_url_dialog_open_clicked (GtkButton *btn, gpointer data)
{
    gpointer *closure = data;
    AdwDialog *dialog = closure[0];
    AdwEntryRow *entry = closure[1];
    VlxWindow *self = closure[2];
    (void) btn;

    const gchar *url = gtk_editable_get_text (GTK_EDITABLE (entry));
    if (url && *url) {
        vlx_playlist_append (self->playlist, url);
        guint n = vlx_playlist_get_length (self->playlist);
        if (vlx_playlist_set_index (self->playlist, n - 1)) {
            vlx_player_open (self->player, url);
            GstElement *sink = vlx_player_get_video_sink (self->player);
            vlx_video_widget_set_sink (self->video_widget, sink);
        }
    }
    adw_dialog_close (dialog);
    g_free (closure);
}

static void
vlx_window_show_open_url_dialog (VlxWindow *self)
{
    AdwDialog *dialog = adw_dialog_new ();
    adw_dialog_set_title (dialog, "Open Network Stream");
    adw_dialog_set_content_width (dialog, 450);

    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 16);
    gtk_widget_set_margin_start (box, 16);
    gtk_widget_set_margin_end (box, 16);
    gtk_widget_set_margin_top (box, 16);
    gtk_widget_set_margin_bottom (box, 16);

    AdwPreferencesGroup *grp = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
    AdwEntryRow *entry = ADW_ENTRY_ROW (adw_entry_row_new ());
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (entry), "URL");
    adw_action_row_set_subtitle (ADW_ACTION_ROW (entry), "HTTP(S), RTSP, HLS, DASH, UDP...");
    adw_preferences_group_add (grp, GTK_WIDGET (entry));
    gtk_box_append (GTK_BOX (box), GTK_WIDGET (grp));

    GtkWidget *btn = gtk_button_new_with_label ("Open");
    gtk_widget_add_css_class (btn, "suggested-action");
    gtk_widget_set_halign (btn, GTK_ALIGN_END);
    gtk_box_append (GTK_BOX (box), btn);

    adw_dialog_set_child (dialog, box);

    gpointer *closure = g_new (gpointer, 3);
    closure[0] = dialog;
    closure[1] = entry;
    closure[2] = self;
    g_signal_connect (btn, "clicked", G_CALLBACK (on_url_dialog_open_clicked), closure);

    adw_dialog_present (dialog, GTK_WIDGET (self));
}

/* ── Keyboard shortcuts ───────────────────────────────────────────────── */
static gboolean
on_key_pressed (GtkEventControllerKey *ctrl,
                guint keyval, guint keycode,
                GdkModifierType state,
                gpointer data)
{
    VlxWindow *self = VLX_WINDOW (data);
    (void) ctrl; (void) keycode; (void) state;

    if ((state & GDK_CONTROL_MASK) && (keyval == GDK_KEY_u || keyval == GDK_KEY_U)) {
        vlx_window_show_open_url_dialog (self);
        return TRUE;
    }
    if ((state & GDK_CONTROL_MASK) && (keyval == GDK_KEY_o || keyval == GDK_KEY_O)) {
        g_action_group_activate_action (G_ACTION_GROUP (self), "open-file", NULL);
        return TRUE;
    }

    switch (keyval) {
    case GDK_KEY_n:
    case GDK_KEY_N:
        play_playlist_uri (self, vlx_playlist_next (self->playlist));
        return TRUE;
    case GDK_KEY_q:
    case GDK_KEY_Q:
        g_action_group_activate_action (G_ACTION_GROUP (self), "toggle-queue", NULL);
        return TRUE;
    case GDK_KEY_b:
    case GDK_KEY_B:
        play_playlist_uri (self, vlx_playlist_previous (self->playlist));
        return TRUE;
    case GDK_KEY_space:
        vlx_player_toggle (self->player);
        vlx_window_show_hud (self, "Play / Pause");
        return TRUE;
    case GDK_KEY_Left:
    case GDK_KEY_j:
    case GDK_KEY_J: {
        gint64 jump = 10;
        if (state & GDK_SHIFT_MASK) jump = 3;
        else if (state & GDK_CONTROL_MASK) jump = 60;
        vlx_player_seek_relative (self->player, -jump * G_USEC_PER_SEC);
        vlx_controls_overlay_seek_ripple (self->controls, FALSE);
        vlx_window_show_hud (self, "-%ds", (int)jump);
        return TRUE;
    }
    case GDK_KEY_Right:
    case GDK_KEY_l:
    case GDK_KEY_L: {
        gint64 jump = 10;
        if (state & GDK_SHIFT_MASK) jump = 3;
        else if (state & GDK_CONTROL_MASK) jump = 60;
        vlx_player_seek_relative (self->player, +jump * G_USEC_PER_SEC);
        vlx_controls_overlay_seek_ripple (self->controls, TRUE);
        vlx_window_show_hud (self, "+%ds", (int)jump);
        return TRUE;
    }
    case GDK_KEY_Up: {
        gdouble vol = CLAMP (vlx_player_get_volume (self->player) + 0.05, 0.0, 1.0);
        vlx_player_set_volume (self->player, vol);
        vlx_window_show_hud (self, "Volume: %d%%", (int)(vol * 100));
        return TRUE;
    }
    case GDK_KEY_Down: {
        gdouble vol = CLAMP (vlx_player_get_volume (self->player) - 0.05, 0.0, 1.0);
        vlx_player_set_volume (self->player, vol);
        vlx_window_show_hud (self, "Volume: %d%%", (int)(vol * 100));
        return TRUE;
    }
    case GDK_KEY_bracketleft: {
        gdouble rate = MAX (0.25, vlx_player_get_rate (self->player) - 0.1);
        vlx_player_set_rate (self->player, rate);
        vlx_window_show_hud (self, "Speed: %.1fx", rate);
        return TRUE;
    }
    case GDK_KEY_bracketright: {
        gdouble rate = MIN (4.0, vlx_player_get_rate (self->player) + 0.1);
        vlx_player_set_rate (self->player, rate);
        vlx_window_show_hud (self, "Speed: %.1fx", rate);
        return TRUE;
    }
    case GDK_KEY_f:
    case GDK_KEY_F11:
        if (gtk_window_is_fullscreen (GTK_WINDOW (self))) {
            gtk_window_unfullscreen (GTK_WINDOW (self));
            vlx_window_show_hud (self, "Exited Fullscreen");
        } else {
            gtk_window_fullscreen (GTK_WINDOW (self));
            vlx_window_show_hud (self, "Fullscreen");
        }
        return TRUE;
    case GDK_KEY_Escape:
        if (gtk_window_is_fullscreen (GTK_WINDOW (self)))
            gtk_window_unfullscreen (GTK_WINDOW (self));
        return TRUE;
    case GDK_KEY_m:
    case GDK_KEY_M: {
        gboolean muted = !vlx_player_get_muted (self->player);
        vlx_player_set_muted (self->player, muted);
        vlx_window_show_hud (self, muted ? "Muted" : "Unmuted");
        return TRUE;
    }
    case GDK_KEY_s: {
        gint64 delay = vlx_player_get_subtitle_delay (self->player) - 50000;
        vlx_player_set_subtitle_delay (self->player, delay);
        vlx_window_show_hud (self, "Subtitle delay: %lld ms", delay / 1000);
        return TRUE;
    }
    case GDK_KEY_S: {
        gint64 delay = vlx_player_get_subtitle_delay (self->player) + 50000;
        vlx_player_set_subtitle_delay (self->player, delay);
        vlx_window_show_hud (self, "Subtitle delay: %lld ms", delay / 1000);
        return TRUE;
    }
    case GDK_KEY_v:
    case GDK_KEY_V:
        vlx_window_show_hud (self, "Video Track (WIP)");
        return TRUE;
    case GDK_KEY_p: {
        gchar *dir = g_build_filename (g_get_user_special_dir (G_USER_DIRECTORY_PICTURES), "Velox", NULL);
        g_mkdir_with_parents (dir, 0755);
        gint64 pos = vlx_player_get_position (self->player);
        gchar *name = g_strdup_printf ("frame_%08ld.png", pos / 1000);
        gchar *path = g_build_filename (dir, name, NULL);
        vlx_video_widget_take_screenshot (self->video_widget, path);
        vlx_window_show_hud (self, "\u2709 Screenshot saved");
        gchar *toast_msg = g_strdup_printf ("Saved to %s", path);
        AdwToast *toast = adw_toast_new (toast_msg);
        adw_toast_overlay_add_toast (self->toast_overlay, toast);
        g_free (toast_msg);
        g_free (path);
        g_free (name);
        g_free (dir);
        return TRUE;
    }
    case GDK_KEY_P:
        /* Ctrl+P is caught above for PiP; bare P is screenshot — but
         * uppercase P (Shift+P) → PiP toggle as well for discoverability. */
        if (self->pip_window && gtk_widget_get_visible (GTK_WIDGET (self->pip_window))) {
            vlx_pip_window_close_pip (self->pip_window);
            vlx_window_show_hud (self, "PiP closed");
        } else {
            if (!self->pip_window)
                self->pip_window = vlx_pip_window_new (self->video_widget);
            vlx_pip_window_present (self->pip_window, GTK_WINDOW (self));
            vlx_window_show_hud (self, "\u2B1C PiP opened");
        }
        return TRUE;
    case GDK_KEY_i:
    case GDK_KEY_I:
        vlx_window_show_media_info (self);
        return TRUE;
    case GDK_KEY_a:
    case GDK_KEY_A:
        vlx_window_show_hud (self, "Audio Track (WIP)");
        return TRUE;
    case GDK_KEY_g: {
        /* A-B Loop: G cycles  (none) → set A → set B (active) → clear */
        gint64 a, b;
        gboolean active = vlx_player_get_ab_state (self->player, &a, &b);
        if (active) {
            vlx_player_clear_ab (self->player);
            vlx_window_show_hud (self, "A-B Loop cleared");
        } else if (a >= 0) {
            vlx_player_set_ab_b (self->player);
            vlx_player_get_ab_state (self->player, &a, &b);
            if (b >= 0)
                vlx_window_show_hud (self, "\u21BB A-B Loop active");
            else
                vlx_window_show_hud (self, "B must be after A");
        } else {
            vlx_player_set_ab_a (self->player);
            vlx_window_show_hud (self, "A set \u2014 press G again for B");
        }
        return TRUE;
    }
    case GDK_KEY_G:
        /* Shift+G = clear A-B loop immediately */
        vlx_player_clear_ab (self->player);
        vlx_window_show_hud (self, "A-B Loop cleared");
        return TRUE;
    case GDK_KEY_c:
        /* Next chapter */
        vlx_player_seek_chapter (self->player, +1);
        vlx_window_show_hud (self, "\u25B6 Next Chapter");
        return TRUE;
    case GDK_KEY_C:
        /* Previous chapter */
        vlx_player_seek_chapter (self->player, -1);
        vlx_window_show_hud (self, "\u25C0 Previous Chapter");
        return TRUE;

    /* ── Video balance ──────────────────────────────────────────────── */
    case GDK_KEY_KP_Add:
    case GDK_KEY_plus:
        /* Brightness up (+0.05, max 1.0) */
        self->brightness = CLAMP (self->brightness + 0.05, -1.0, 1.0);
        vlx_player_set_brightness (self->player, self->brightness);
        vlx_window_show_hud (self, g_strdup_printf ("\u2600 Brightness: %+.0f%%",
                                                     self->brightness * 100));
        return TRUE;
    case GDK_KEY_KP_Subtract:
    case GDK_KEY_minus:
        /* Brightness down */
        self->brightness = CLAMP (self->brightness - 0.05, -1.0, 1.0);
        vlx_player_set_brightness (self->player, self->brightness);
        vlx_window_show_hud (self, g_strdup_printf ("\u2600 Brightness: %+.0f%%",
                                                     self->brightness * 100));
        return TRUE;
    case GDK_KEY_question:
        /* ? = keyboard cheat sheet */
        vlx_window_show_cheat_sheet (self);
        return TRUE;

    default:
        return FALSE;
    }
}

static gboolean
on_key_released (GtkEventControllerKey *ctrl,
                 guint keyval, guint keycode,
                 GdkModifierType state,
                 gpointer data)
{
    (void) ctrl; (void) keycode; (void) state; (void) data;
    if (keyval == GDK_KEY_space || keyval == GDK_KEY_Return || keyval == GDK_KEY_Escape)
        return TRUE;
    return FALSE;
}

/* ── Subtitle drag-and-drop ───────────────────────────────────────────── */
static gboolean
on_subtitle_drop (GtkDropTarget *target,
                  const GValue  *value,
                  gdouble        x,
                  gdouble        y,
                  gpointer       data)
{
    VlxWindow *self = VLX_WINDOW (data);
    (void) target; (void) x; (void) y;

    if (!G_VALUE_HOLDS (value, G_TYPE_FILE)) return FALSE;

    GFile *file = g_value_get_object (value);
    gchar *path = g_file_get_path (file);
    if (!path) return FALSE;

    /* Accept subtitle extensions only */
    if (g_str_has_suffix (path, ".srt") ||
        g_str_has_suffix (path, ".ass") ||
        g_str_has_suffix (path, ".ssa") ||
        g_str_has_suffix (path, ".vtt")) {
        vlx_player_load_subtitle_file (self->player, path);
        vlx_window_show_hud (self, "📄 Subtitle loaded");
    } else {
        /* Treat as a media file drop */
        gchar *uri = g_file_get_uri (file);
        vlx_window_open (self, uri);
        g_free (uri);
    }

    g_free (path);
    return TRUE;
}

/* ── Keyboard cheat sheet (? key) ────────────────────────────────────── */
static void
vlx_window_show_cheat_sheet (VlxWindow *self)
{
    AdwDialog *dlg = ADW_DIALOG (adw_dialog_new ());
    adw_dialog_set_title (dlg, "Keyboard Shortcuts");
    adw_dialog_set_content_width  (dlg, 540);
    adw_dialog_set_content_height (dlg, 560);

    GtkWidget *toolbar_view = adw_toolbar_view_new ();
    GtkWidget *header = GTK_WIDGET (adw_header_bar_new ());
    adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar_view), header);

    GtkWidget *scroll = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll),
                                    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

    /* Build shortcut table */
    static const struct { const gchar *key; const gchar *action; } shortcuts[] = {
        { "Space",       "Play / Pause"          },
        { "← / j",      "Seek -10 s"            },
        { "→ / l",      "Seek +10 s"            },
        { "↑",          "Volume +5%"            },
        { "↓",          "Volume -5%"            },
        { "[ / ]",      "Speed -/+ 0.1×"        },
        { "m",          "Mute toggle"           },
        { "f / F11",    "Fullscreen"            },
        { "n / b",      "Next / Prev track"     },
        { "s / S",      "Subtitle delay ±50 ms" },
        { "p",          "Screenshot"            },
        { "P",          "Picture-in-Picture"    },
        { "g",          "Set A / B / clear loop"},
        { "G",          "Clear A-B loop"        },
        { "c / C",      "Next / Prev chapter"   },
        { "+ / -",      "Brightness ±5%"        },
        { "i",          "Media info"            },
        { "Ctrl+O",     "Open file"             },
        { "Ctrl+U",     "Open URL"              },
        { "?",          "This help sheet"       },
    };

    GtkWidget *grid = gtk_grid_new ();
    gtk_grid_set_column_spacing (GTK_GRID (grid), 24);
    gtk_grid_set_row_spacing    (GTK_GRID (grid), 6);
    gtk_widget_set_margin_start  (grid, 24);
    gtk_widget_set_margin_end    (grid, 24);
    gtk_widget_set_margin_top    (grid, 16);
    gtk_widget_set_margin_bottom (grid, 16);

    for (guint i = 0; i < G_N_ELEMENTS (shortcuts); i++) {
        GtkWidget *key_lbl = gtk_label_new (shortcuts[i].key);
        gtk_widget_add_css_class (key_lbl, "monospace");
        gtk_label_set_xalign (GTK_LABEL (key_lbl), 1.0f);
        gtk_widget_set_hexpand (key_lbl, FALSE);

        GtkWidget *act_lbl = gtk_label_new (shortcuts[i].action);
        gtk_label_set_xalign (GTK_LABEL (act_lbl), 0.0f);
        gtk_widget_set_hexpand (act_lbl, TRUE);

        gtk_grid_attach (GTK_GRID (grid), key_lbl, 0, (gint)i, 1, 1);
        gtk_grid_attach (GTK_GRID (grid), act_lbl, 1, (gint)i, 1, 1);
    }

    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), grid);
    adw_toolbar_view_set_content  (ADW_TOOLBAR_VIEW (toolbar_view), scroll);
    adw_dialog_set_child (dlg, toolbar_view);

    adw_dialog_present (dlg, GTK_WIDGET (self));
}

/* ── GObject boilerplate ──────────────────────────────────────────────── */
static void
vlx_window_dispose (GObject *obj)
{
    VlxWindow *self = VLX_WINDOW (obj);

    /* Save watch position before shutting down */
    if (self->history && self->player) {
        const gchar *uri = vlx_player_get_uri (self->player);
        if (uri) {
            gint64 pos = vlx_player_get_position (self->player);
            vlx_history_update_position (self->history, uri, pos);
        }
    }

    if (self->pip_window) {
        vlx_pip_window_close_pip (self->pip_window);
        g_clear_object (&self->pip_window);
    }
    g_clear_object (&self->collection);
    g_clear_object (&self->player);
    g_clear_object (&self->playlist);
    G_OBJECT_CLASS (vlx_window_parent_class)->dispose (obj);
}

static void
vlx_window_class_init (VlxWindowClass *klass)
{
    G_OBJECT_CLASS (klass)->dispose = vlx_window_dispose;
}

static void
vlx_window_init (VlxWindow *self)
{
    /* ── Core objects ─────────────────────────────────────────────── */
    self->player   = vlx_player_new ();
    self->playlist = vlx_playlist_new ();
    self->bus      = vlx_event_bus_get_default ();
    self->history  = vlx_history_get_default ();

    /* P4 — balance defaults */
    self->brightness = 0.0;
    self->contrast   = 1.0;
    self->saturation = 1.0;
    self->hue        = 0.0;

    /* ── Header bar ───────────────────────────────────────────────── */
    self->hbar = GTK_WIDGET (adw_header_bar_new ());

    self->open_btn = gtk_button_new_from_icon_name ("document-open-symbolic");
    gtk_widget_set_tooltip_text (self->open_btn, "Open file (Ctrl+O)");
    adw_header_bar_pack_start (ADW_HEADER_BAR (self->hbar), self->open_btn);

    self->queue_btn = gtk_toggle_button_new ();
    gtk_button_set_icon_name (GTK_BUTTON (self->queue_btn), "view-list-symbolic");
    gtk_widget_set_tooltip_text (self->queue_btn, "Toggle Queue (Q)");
    adw_header_bar_pack_end (ADW_HEADER_BAR (self->hbar), self->queue_btn);

    self->menu_btn = gtk_menu_button_new ();
    gtk_menu_button_set_icon_name (GTK_MENU_BUTTON (self->menu_btn),
                                   "open-menu-symbolic");
    adw_header_bar_pack_end (ADW_HEADER_BAR (self->hbar), self->menu_btn);

    /* ── Wire up hamburger menu with all actions ──────────────────── */
    vlx_window_setup_actions_and_menu (self);

    /* ── Child widgets ────────────────────────────────────────────── */
    self->video_widget     = vlx_video_widget_new ();
    self->controls         = vlx_controls_overlay_new (self->player);
    self->subtitle_overlay = vlx_subtitle_overlay_new ();
    self->playlist_panel   = vlx_playlist_panel_new (self->playlist);

    /* ── Overlay stack: video → subtitle → spinner → controls → HUD ─────────── */
    self->overlay = gtk_overlay_new ();
    gtk_overlay_set_child (GTK_OVERLAY (self->overlay),
                           GTK_WIDGET (self->video_widget));
    gtk_overlay_add_overlay (GTK_OVERLAY (self->overlay),
                             GTK_WIDGET (self->subtitle_overlay));

    self->spinner = gtk_spinner_new ();
    gtk_spinner_start (GTK_SPINNER (self->spinner));
    gtk_widget_set_halign (self->spinner, GTK_ALIGN_CENTER);
    gtk_widget_set_valign (self->spinner, GTK_ALIGN_CENTER);
    gtk_widget_set_size_request (self->spinner, 64, 64);
    gtk_widget_set_visible (self->spinner, FALSE);
    gtk_overlay_add_overlay (GTK_OVERLAY (self->overlay), self->spinner);

    gtk_overlay_add_overlay (GTK_OVERLAY (self->overlay),
                             GTK_WIDGET (self->controls));

    /* HUD pill */
    self->hud_label = gtk_label_new ("");
    gtk_widget_add_css_class (self->hud_label, "osd");
    gtk_widget_set_halign (self->hud_label, GTK_ALIGN_END);
    gtk_widget_set_valign (self->hud_label, GTK_ALIGN_START);
    gtk_widget_set_margin_top (self->hud_label, 24);
    gtk_widget_set_margin_end (self->hud_label, 24);
    gtk_widget_set_opacity (self->hud_label, 0.0);
    
    /* GTK4 native CSS transition for opacity */
    GtkCssProvider *css = gtk_css_provider_new ();
    gtk_css_provider_load_from_string (css, ".osd { transition: opacity 200ms ease; background: rgba(0,0,0,0.7); color: white; border-radius: 99px; padding: 8px 16px; font-weight: bold; }");
    gtk_style_context_add_provider_for_display (gdk_display_get_default (),
                                                GTK_STYLE_PROVIDER (css),
                                                GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref (css);

    gtk_overlay_add_overlay (GTK_OVERLAY (self->overlay), self->hud_label);

    gtk_widget_set_hexpand (self->overlay, TRUE);
    gtk_widget_set_vexpand (self->overlay, TRUE);

    /* ── Split view: sidebar (playlist) + content (video) ─────────── */
    self->split = ADW_OVERLAY_SPLIT_VIEW (
        adw_overlay_split_view_new ());
    adw_overlay_split_view_set_collapsed (self->split, TRUE);
    adw_overlay_split_view_set_show_sidebar (self->split, FALSE);

    /* Sidebar */
    adw_overlay_split_view_set_sidebar (self->split, GTK_WIDGET (self->playlist_panel));

    /* Content */
    adw_overlay_split_view_set_content (self->split, self->overlay);

    /* ── Toast Overlay ────────────────────────────────────────────── */
    self->toast_overlay = ADW_TOAST_OVERLAY (adw_toast_overlay_new ());
    adw_toast_overlay_set_child (self->toast_overlay, GTK_WIDGET (self->split));

    /* ── Main layout: toolbar + toast_overlay ─────────────────────── */
    self->toolbar_view = ADW_TOOLBAR_VIEW (adw_toolbar_view_new ());
    adw_toolbar_view_add_top_bar (self->toolbar_view, self->hbar);
    adw_toolbar_view_set_content (self->toolbar_view, GTK_WIDGET (self->toast_overlay));

    adw_application_window_set_content (ADW_APPLICATION_WINDOW (self),
                                        GTK_WIDGET (self->toolbar_view));

    /* ── Controllers ─────────────────────────────────────────────── */
    GtkEventController *motion = gtk_event_controller_motion_new ();
    g_signal_connect (motion, "motion", G_CALLBACK (on_motion), self);
    gtk_widget_add_controller (GTK_WIDGET (self), motion);

    GtkEventController *key = gtk_event_controller_key_new ();
    gtk_event_controller_set_propagation_phase (key, GTK_PHASE_CAPTURE);
    g_signal_connect (key, "key-pressed", G_CALLBACK (on_key_pressed), self);
    g_signal_connect (key, "key-released", G_CALLBACK (on_key_released), self);
    gtk_widget_add_controller (GTK_WIDGET (self), key);

    /* ── Signals ─────────────────────────────────────────────────── */
    g_signal_connect_object (self->bus, "state-changed",
                             G_CALLBACK (on_state_changed), self, 0);
    g_signal_connect (self->open_btn, "clicked",
                      G_CALLBACK (on_open_clicked), self);
    g_signal_connect (self->playlist_panel, "item-activated",
                      G_CALLBACK (on_playlist_item_activated), self);
    g_signal_connect (self->controls, "next-video",
                      G_CALLBACK (on_controls_next), self);
    g_signal_connect (self->controls, "prev-video",
                      G_CALLBACK (on_controls_prev), self);
    g_signal_connect (self, "notify::fullscreened",
                      G_CALLBACK (on_fullscreened), NULL);
    g_signal_connect_object (self->bus, "eos",
                             G_CALLBACK (on_eos), self, 0);
    g_signal_connect_object (self->bus, "stream-collection",
                             G_CALLBACK (on_stream_collection), self, 0);

    /* Bind queue button active state to split view show-sidebar */
    g_object_bind_property (self->queue_btn, "active",
                            self->split, "show-sidebar",
                            G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE);

    /* ── Click-to-play/pause on video area ── */
    GtkGesture *click = gtk_gesture_click_new ();
    gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (click), GDK_BUTTON_PRIMARY);
    g_signal_connect (click, "released", G_CALLBACK (on_video_clicked), self);
    gtk_widget_add_controller (self->overlay, GTK_EVENT_CONTROLLER (click));

    /* Make video overlay focusable so Space always routes to our key handler */
    gtk_widget_set_focusable (self->overlay, TRUE);
    gtk_widget_grab_focus (self->overlay);

    /* ── Subtitle file drag-and-drop ── */
    GtkDropTarget *drop = gtk_drop_target_new (G_TYPE_FILE, GDK_ACTION_COPY);
    g_signal_connect (drop, "drop", G_CALLBACK (on_subtitle_drop), self);
    gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (drop));
}

/* ── HUD ─────────────────────────────────────────────────────────────── */
VlxWindow *
vlx_window_new (AdwApplication *app)
{
    return g_object_new (VLX_TYPE_WINDOW,
                         "application",    app,
                         "title",          "Velox",
                         "default-width",  1280,
                         "default-height", 720,
                         NULL);
}

void
vlx_window_open (VlxWindow *self, const gchar *uri)
{
    g_return_if_fail (VLX_IS_WINDOW (self));
    g_return_if_fail (uri != NULL);

    /* Save position of previous file before switching */
    if (self->history) {
        const gchar *prev = vlx_player_get_uri (self->player);
        if (prev) {
            gint64 pos = vlx_player_get_position (self->player);
            vlx_history_update_position (self->history, prev, pos);
        }
    }

    vlx_playlist_append (self->playlist, uri);
    vlx_player_open (self->player, uri);

    GstElement *sink = vlx_player_get_video_sink (self->player);
    vlx_video_widget_set_sink (self->video_widget, sink);

    /* Record in watch history */
    if (self->history) {
        vlx_history_record (self->history, uri, NULL,
                            vlx_player_get_duration (self->player));

        /* Resume from last position if available */
        gint64 resume = vlx_history_get_resume_pos (self->history, uri);
        if (resume > 0) {
            vlx_player_seek (self->player, resume);
            vlx_window_show_hud (self, "\u23E9 Resuming");
        }
    }
}

VlxPlayer *
vlx_window_get_player (VlxWindow *self)
{
    g_return_val_if_fail (VLX_IS_WINDOW (self), NULL);
    return self->player;
}

VlxPlaylist *
vlx_window_get_playlist (VlxWindow *self)
{
    g_return_val_if_fail (VLX_IS_WINDOW (self), NULL);
    return self->playlist;
}
