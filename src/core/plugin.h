/* vlx_plugin.h — Plugin interface for Velox
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A Velox plugin is a shared library (.so) that exports three symbols:
 *
 *   const VlxPluginInfo *vlx_plugin_get_info (void);
 *   gboolean             vlx_plugin_init     (VlxPluginHost *host);
 *   void                 vlx_plugin_shutdown (void);
 *
 * The host provides access to the player, event bus, and UI hooks so
 * plugins can react to playback events, add menu items, or inject
 * GStreamer elements into the pipeline.
 *
 * Plugin search path:
 *   1. $XDG_DATA_HOME/velox/plugins/           (user)
 *   2. $datadir/velox/plugins/                   (system)
 *   3. $VELOX_PLUGIN_PATH (colon-separated)     (developer)
 */

#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

/* ── Plugin metadata (returned by vlx_plugin_get_info) ─────────────────────── */
typedef struct {
    const gchar *id;           /* unique reverse-dns, e.g. "io.velox.equalizer" */
    const gchar *name;         /* human-readable name */
    const gchar *description;  /* one-line summary */
    const gchar *version;      /* semver string */
    const gchar *author;       /* display name or email */
    guint        api_version;  /* must match VLX_PLUGIN_API_VERSION */
} VlxPluginInfo;

#define VLX_PLUGIN_API_VERSION 1

/* ── Host interface (provided to plugin on init) ───────────────────────────── */
typedef struct _VlxPluginHost VlxPluginHost;

/* Accessor functions — plugins call these instead of importing internals */
GObject *vlx_plugin_host_get_player    (VlxPluginHost *host);
GObject *vlx_plugin_host_get_event_bus (VlxPluginHost *host);

/* Log through the host so messages get the plugin's domain tag */
void     vlx_plugin_host_log          (VlxPluginHost  *host,
                                        const gchar    *domain,
                                        const gchar    *format,
                                        ...) G_GNUC_PRINTF (3, 4);

/* ── Function signatures that every plugin must export ─────────────────────── */
typedef const VlxPluginInfo *(*VlxPluginGetInfoFunc)  (void);
typedef gboolean             (*VlxPluginInitFunc)     (VlxPluginHost *host);
typedef void                 (*VlxPluginShutdownFunc) (void);

/* ── Plugin manager (singleton) ────────────────────────────────────────────── */
#define VLX_TYPE_PLUGIN_MANAGER (vlx_plugin_manager_get_type ())
G_DECLARE_FINAL_TYPE (VlxPluginManager, vlx_plugin_manager,
                      VLX, PLUGIN_MANAGER, GObject)

VlxPluginManager *vlx_plugin_manager_new (GObject *player,
                                          GObject *event_bus);

/* Scan directories and load all valid plugins */
void              vlx_plugin_manager_load_all    (VlxPluginManager *self);

/* Unload all plugins (called on shutdown) */
void              vlx_plugin_manager_unload_all  (VlxPluginManager *self);

/* Query loaded plugins */
guint             vlx_plugin_manager_get_count   (VlxPluginManager *self);
const VlxPluginInfo *vlx_plugin_manager_get_info (VlxPluginManager *self,
                                                   guint             index);

G_END_DECLS
