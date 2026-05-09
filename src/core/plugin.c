/* vlx_plugin.c — Plugin loader and manager
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/plugin.h"
#include "utils/log.h"

#include <gmodule.h>
#include <gio/gio.h>

#define VLX_LOG_DOMAIN_PLUGIN "Velox-Plugin"

/* ── Host object (given to each plugin) ──────────────────────────────────── */
struct _VlxPluginHost {
    GObject *player;
    GObject *event_bus;
};

GObject *
vlx_plugin_host_get_player (VlxPluginHost *host)
{
    g_return_val_if_fail (host != NULL, NULL);
    return host->player;
}

GObject *
vlx_plugin_host_get_event_bus (VlxPluginHost *host)
{
    g_return_val_if_fail (host != NULL, NULL);
    return host->event_bus;
}

void
vlx_plugin_host_log (VlxPluginHost *host, const gchar *domain,
                     const gchar *format, ...)
{
    (void) host;
    va_list args;
    va_start (args, format);
    gchar *msg = g_strdup_vprintf (format, args);
    va_end (args);
    VLX_LOG_INFO (domain ? domain : VLX_LOG_DOMAIN_PLUGIN, "%s", msg);
    g_free (msg);
}

/* ── Loaded plugin descriptor ────────────────────────────────────────────── */
typedef struct {
    GModule              *module;
    const VlxPluginInfo  *info;
    VlxPluginShutdownFunc shutdown_fn;
} LoadedPlugin;

/* ── Manager ─────────────────────────────────────────────────────────────── */
struct _VlxPluginManager {
    GObject         parent_instance;
    VlxPluginHost   host;
    GPtrArray      *plugins;   /* LoadedPlugin* */
};

G_DEFINE_TYPE (VlxPluginManager, vlx_plugin_manager, G_TYPE_OBJECT)

static void
loaded_plugin_free (gpointer data)
{
    LoadedPlugin *lp = data;
    if (lp->shutdown_fn)
        lp->shutdown_fn ();
    if (lp->module)
        g_module_close (lp->module);
    g_free (lp);
}

static void
vlx_plugin_manager_finalize (GObject *obj)
{
    VlxPluginManager *self = VLX_PLUGIN_MANAGER (obj);
    g_ptr_array_unref (self->plugins);
    G_OBJECT_CLASS (vlx_plugin_manager_parent_class)->finalize (obj);
}

static void
vlx_plugin_manager_class_init (VlxPluginManagerClass *klass)
{
    G_OBJECT_CLASS (klass)->finalize = vlx_plugin_manager_finalize;
}

static void
vlx_plugin_manager_init (VlxPluginManager *self)
{
    self->plugins = g_ptr_array_new_with_free_func (loaded_plugin_free);
}

VlxPluginManager *
vlx_plugin_manager_new (GObject *player, GObject *event_bus)
{
    VlxPluginManager *self = g_object_new (VLX_TYPE_PLUGIN_MANAGER, NULL);
    self->host.player    = player;
    self->host.event_bus = event_bus;
    return self;
}

/* ── Load a single .so ───────────────────────────────────────────────────── */
static gboolean
try_load_plugin (VlxPluginManager *self, const gchar *path)
{
    GModule *module = g_module_open (path, G_MODULE_BIND_LAZY);
    if (!module) {
        VLX_LOG_WARNING (VLX_LOG_DOMAIN_PLUGIN,
                         "Cannot open %s: %s", path, g_module_error ());
        return FALSE;
    }

    VlxPluginGetInfoFunc  get_info_fn  = NULL;
    VlxPluginInitFunc     init_fn      = NULL;
    VlxPluginShutdownFunc shutdown_fn  = NULL;

    if (!g_module_symbol (module, "vlx_plugin_get_info",
                           (gpointer *) &get_info_fn) ||
        !g_module_symbol (module, "vlx_plugin_init",
                           (gpointer *) &init_fn)) {
        VLX_LOG_WARNING (VLX_LOG_DOMAIN_PLUGIN,
                         "Missing required symbols in %s", path);
        g_module_close (module);
        return FALSE;
    }

    /* Shutdown is optional */
    g_module_symbol (module, "vlx_plugin_shutdown",
                     (gpointer *) &shutdown_fn);

    const VlxPluginInfo *info = get_info_fn ();
    if (!info || info->api_version != VLX_PLUGIN_API_VERSION) {
        VLX_LOG_WARNING (VLX_LOG_DOMAIN_PLUGIN,
                         "API version mismatch in %s (got %u, need %u)",
                         path,
                         info ? info->api_version : 0,
                         VLX_PLUGIN_API_VERSION);
        g_module_close (module);
        return FALSE;
    }

    /* Check for duplicate */
    for (guint i = 0; i < self->plugins->len; i++) {
        LoadedPlugin *existing = g_ptr_array_index (self->plugins, i);
        if (g_strcmp0 (existing->info->id, info->id) == 0) {
            VLX_LOG_WARNING (VLX_LOG_DOMAIN_PLUGIN,
                             "Skipping duplicate plugin: %s", info->id);
            g_module_close (module);
            return FALSE;
        }
    }

    /* Initialise */
    VLX_LOG_INFO (VLX_LOG_DOMAIN_PLUGIN,
                  "Loading plugin: %s v%s (%s)",
                  info->name, info->version, info->id);

    if (!init_fn (&self->host)) {
        VLX_LOG_WARNING (VLX_LOG_DOMAIN_PLUGIN,
                         "Plugin %s init failed", info->id);
        g_module_close (module);
        return FALSE;
    }

    LoadedPlugin *lp   = g_new0 (LoadedPlugin, 1);
    lp->module         = module;
    lp->info           = info;
    lp->shutdown_fn    = shutdown_fn;
    g_ptr_array_add (self->plugins, lp);

    VLX_LOG_INFO (VLX_LOG_DOMAIN_PLUGIN,
                  "Plugin loaded: %s", info->name);
    return TRUE;
}

/* ── Scan a directory for .so files ──────────────────────────────────────── */
static void
scan_dir (VlxPluginManager *self, const gchar *dir_path)
{
    GDir *dir = g_dir_open (dir_path, 0, NULL);
    if (!dir) return;

    VLX_LOG_INFO (VLX_LOG_DOMAIN_PLUGIN, "Scanning: %s", dir_path);

    const gchar *name;
    while ((name = g_dir_read_name (dir)) != NULL) {
        if (!g_str_has_suffix (name, ".so")) continue;
        gchar *full = g_build_filename (dir_path, name, NULL);
        try_load_plugin (self, full);
        g_free (full);
    }
    g_dir_close (dir);
}

/* ── Public API ──────────────────────────────────────────────────────────── */
void
vlx_plugin_manager_load_all (VlxPluginManager *self)
{
    g_return_if_fail (VLX_IS_PLUGIN_MANAGER (self));

    /* 1. User plugins */
    gchar *user_dir = g_build_filename (g_get_user_data_dir (),
                                         "velox", "plugins", NULL);
    g_mkdir_with_parents (user_dir, 0700);
    scan_dir (self, user_dir);
    g_free (user_dir);

    /* 2. System plugins (based on install prefix) */
    gchar *sys_dir = g_build_filename (VELOX_DATADIR ? VELOX_DATADIR : "/usr/share",
                                        "velox", "plugins", NULL);
    scan_dir (self, sys_dir);
    g_free (sys_dir);

    /* 3. VELOX_PLUGIN_PATH env var (colon-separated) */
    const gchar *env = g_getenv ("VELOX_PLUGIN_PATH");
    if (env) {
        gchar **paths = g_strsplit (env, ":", -1);
        for (guint i = 0; paths[i]; i++)
            scan_dir (self, paths[i]);
        g_strfreev (paths);
    }

    VLX_LOG_INFO (VLX_LOG_DOMAIN_PLUGIN,
                  "%u plugin(s) loaded", self->plugins->len);
}

void
vlx_plugin_manager_unload_all (VlxPluginManager *self)
{
    g_return_if_fail (VLX_IS_PLUGIN_MANAGER (self));
    VLX_LOG_INFO (VLX_LOG_DOMAIN_PLUGIN,
                  "Unloading %u plugin(s)", self->plugins->len);
    g_ptr_array_set_size (self->plugins, 0);
}

guint
vlx_plugin_manager_get_count (VlxPluginManager *self)
{
    g_return_val_if_fail (VLX_IS_PLUGIN_MANAGER (self), 0);
    return self->plugins->len;
}

const VlxPluginInfo *
vlx_plugin_manager_get_info (VlxPluginManager *self, guint index)
{
    g_return_val_if_fail (VLX_IS_PLUGIN_MANAGER (self), NULL);
    g_return_val_if_fail (index < self->plugins->len, NULL);
    LoadedPlugin *lp = g_ptr_array_index (self->plugins, index);
    return lp->info;
}
