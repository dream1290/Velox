/* main.c — Velox Video Player entry point
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Responsibilities:
 *  1. Initialise GStreamer and GTK4/libadwaita.
 *  2. Construct the GApplication lifecycle.
 *  3. Wire platform services (MPRIS2, inhibit, settings).
 *  4. Open files passed on the command line or via GApplication::open.
 *  5. Apply the colour scheme from GSettings.
 */

#include "velox.h"

#include <gst/gst.h>
#include <adwaita.h>
#include <locale.h>

/* ── Application subclass ────────────────────────────────────────────── */
#define VLX_TYPE_APPLICATION (vlx_application_get_type ())
G_DECLARE_FINAL_TYPE (VlxApplication, vlx_application,
                      VLX, APPLICATION, AdwApplication)

struct _VlxApplication {
    AdwApplication    parent_instance;

    VlxWindow        *window;
    VlxMprisProvider *mpris;
    VlxInhibitManager *inhibit;
    VlxPluginManager *plugin_mgr;

};

G_DEFINE_TYPE (VlxApplication, vlx_application, ADW_TYPE_APPLICATION)

/* ── Activate (no URI given) ─────────────────────────────────────────── */
static void
vlx_application_activate (GApplication *app)
{
    VlxApplication *self = VLX_APPLICATION (app);

    if (!self->window)
        self->window = vlx_window_new (ADW_APPLICATION (app));

    gtk_window_present (GTK_WINDOW (self->window));
}

/* ── Open (URI list passed via CLI or D-Bus activation) ──────────────── */
static void
vlx_application_open (GApplication  *app,
                       GFile        **files,
                       gint           n_files,
                       const gchar   *hint)
{
    VlxApplication *self = VLX_APPLICATION (app);
    (void) hint;

    vlx_application_activate (app);

    for (gint i = 0; i < n_files; i++) {
        gchar *uri = g_file_get_uri (files[i]);
        vlx_window_open (self->window, uri);
        g_free (uri);
    }
}

/* ── Startup ─────────────────────────────────────────────────────────── */
static void
vlx_application_startup (GApplication *app)
{
    VlxApplication *self = VLX_APPLICATION (app);

    G_APPLICATION_CLASS (vlx_application_parent_class)->startup (app);

    /* Logging */
    vlx_log_init ();

    /* Thread pool (4 workers for thumbnails + metadata) */
    vlx_thread_pool_init (4);

    /* Apply colour scheme */
    GSettings *settings = g_settings_new ("io.github.velox");
    AdwStyleManager *sm = adw_style_manager_get_default ();
    if (g_settings_get_boolean (settings, "dark-mode"))
        adw_style_manager_set_color_scheme (sm, ADW_COLOR_SCHEME_FORCE_DARK);
    else
        adw_style_manager_set_color_scheme (sm, ADW_COLOR_SCHEME_PREFER_LIGHT);
    g_object_unref (settings);

    /* Inhibit manager */
    self->inhibit = vlx_inhibit_manager_new ();

    /* Sleep inhibitor — wired to event bus in on_window_created
     * once the player exists.  Nothing to do here yet. */

    VLX_LOG_INFO (VLX_LOG_DOMAIN_CORE,
                  "Velox %s starting up", VELOX_VERSION);
}

static void
on_player_state_for_inhibit (VlxEventBus   *bus,
                              VlxPlayerState state,
                              gpointer       data)
{
    VlxInhibitManager *inh = VLX_INHIBIT_MANAGER (data);
    (void) bus;

    if (state == VLX_STATE_PLAYING)
        vlx_inhibit_manager_inhibit (inh);
    else
        vlx_inhibit_manager_uninhibit (inh);
}

/* ── Shutdown ────────────────────────────────────────────────────────── */
static void
vlx_application_shutdown (GApplication *app)
{
    VlxApplication *self = VLX_APPLICATION (app);

    if (self->mpris)
        vlx_mpris_provider_stop (self->mpris);

    if (self->plugin_mgr)
        vlx_plugin_manager_unload_all (self->plugin_mgr);

    vlx_inhibit_manager_uninhibit (self->inhibit);
    vlx_thumbnail_cache_shutdown ();
    vlx_thread_pool_shutdown ();

    G_APPLICATION_CLASS (vlx_application_parent_class)->shutdown (app);
}

/* ── Window created → wire MPRIS ──────────────────────────────────────── */
static void
on_window_created (VlxApplication *self)
{
    if (!self->window) return;
    if (self->mpris) return;

    VlxPlayer *player = vlx_window_get_player (self->window);

    /* MPRIS2 */
    self->mpris = vlx_mpris_provider_new (player);
    vlx_mpris_provider_start (self->mpris);

    /* Inhibit wiring (proper named function) */
    VlxEventBus *bus = vlx_event_bus_get_default ();
    g_signal_connect_object (bus, "state-changed",
                             G_CALLBACK (on_player_state_for_inhibit),
                             self->inhibit, 0);

    /* Plugin manager */
    if (!self->plugin_mgr) {
        self->plugin_mgr = vlx_plugin_manager_new (
            G_OBJECT (player), G_OBJECT (bus));
        vlx_plugin_manager_load_all (self->plugin_mgr);
    }
}

/* ── GObject boilerplate ─────────────────────────────────────────────── */
static void
vlx_application_dispose (GObject *obj)
{
    VlxApplication *self = VLX_APPLICATION (obj);
    g_clear_object (&self->mpris);
    g_clear_object (&self->inhibit);
    g_clear_object (&self->plugin_mgr);
    G_OBJECT_CLASS (vlx_application_parent_class)->dispose (obj);
}

static void
vlx_application_class_init (VlxApplicationClass *klass)
{
    GObjectClass     *obj   = G_OBJECT_CLASS (klass);
    GApplicationClass *app  = G_APPLICATION_CLASS (klass);

    obj->dispose       = vlx_application_dispose;
    app->activate      = vlx_application_activate;
    app->open          = vlx_application_open;
    app->startup       = vlx_application_startup;
    app->shutdown      = vlx_application_shutdown;
}

static void
vlx_application_init (VlxApplication *self)
{
    g_application_set_flags (G_APPLICATION (self),
                             G_APPLICATION_HANDLES_OPEN);
}

/* ── main() ──────────────────────────────────────────────────────────── */
int
main (int argc, char *argv[])
{
    setlocale (LC_ALL, "");

    /* Init GStreamer before GTK so GST_DEBUG works */
    gst_init (&argc, &argv);

    VlxApplication *app = g_object_new (VLX_TYPE_APPLICATION,
                                        "application-id", VLX_APPLICATION_ID,
                                        NULL);

    /* Defer MPRIS wiring until window is created */
    g_signal_connect_swapped (app, "notify::active-window",
                              G_CALLBACK (on_window_created), app);

    int status = g_application_run (G_APPLICATION (app), argc, argv);
    g_object_unref (app);

    gst_deinit ();
    return status;
}
