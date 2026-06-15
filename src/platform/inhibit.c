/* vlx_inhibit.c — Sleep inhibitor via org.freedesktop.PowerManagement
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "platform/inhibit.h"
#include "utils/log.h"

#include <gio/gio.h>

#define PM_BUS   "org.freedesktop.ScreenSaver"
#define PM_PATH  "/org/freedesktop/ScreenSaver"
#define PM_IFACE "org.freedesktop.ScreenSaver"

struct _VlxInhibitManager {
    GObject parent_instance;
    guint   cookie;       /* inhibit cookie, 0 = not inhibiting */
};

G_DEFINE_TYPE (VlxInhibitManager, vlx_inhibit_manager, G_TYPE_OBJECT)

static void
vlx_inhibit_manager_class_init (VlxInhibitManagerClass *klass) { (void) klass; }
static void
vlx_inhibit_manager_init (VlxInhibitManager *self) { self->cookie = 0; }

VlxInhibitManager *
vlx_inhibit_manager_new (void)
{
    return g_object_new (VLX_TYPE_INHIBIT_MANAGER, NULL);
}

void
vlx_inhibit_manager_inhibit (VlxInhibitManager *self)
{
    g_return_if_fail (VLX_IS_INHIBIT_MANAGER (self));
    if (self->cookie != 0) return;

    GError    *err  = NULL;
    GVariant  *res  = NULL;
    GDBusConnection *conn = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &err);
    if (!conn) goto out;

    res = g_dbus_connection_call_sync (
        conn,
        PM_BUS, PM_PATH, PM_IFACE,
        "Inhibit",
        g_variant_new ("(ss)", "Velox", "Video playback"),
        G_VARIANT_TYPE ("(u)"),
        G_DBUS_CALL_FLAGS_NONE,
        3000, NULL, &err);

    if (res) {
        g_variant_get (res, "(u)", &self->cookie);
        g_variant_unref (res);
        VLX_LOG_INFO (VLX_LOG_DOMAIN_PLATFORM,
                      "Sleep inhibited (cookie=%u)", self->cookie);
    }

out:
    if (err) {
        VLX_LOG_WARNING (VLX_LOG_DOMAIN_PLATFORM,
                         "Failed to inhibit sleep: %s", err->message);
        g_error_free (err);
    }
    g_clear_object (&conn);
}

void
vlx_inhibit_manager_uninhibit (VlxInhibitManager *self)
{
    g_return_if_fail (VLX_IS_INHIBIT_MANAGER (self));
    if (self->cookie == 0) return;

    GError          *err  = NULL;
    GDBusConnection *conn = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &err);
    if (!conn) goto out;

    GVariant *res = g_dbus_connection_call_sync (
        conn,
        PM_BUS, PM_PATH, PM_IFACE,
        "UnInhibit",
        g_variant_new ("(u)", self->cookie),
        NULL,
        G_DBUS_CALL_FLAGS_NONE,
        3000, NULL, &err);
    if (res)
        g_variant_unref (res);

    VLX_LOG_INFO (VLX_LOG_DOMAIN_PLATFORM,
                  "Sleep uninhibited (cookie=%u)", self->cookie);
    self->cookie = 0;

out:
    if (err) {
        g_error_free (err);
    }
    g_clear_object (&conn);
}
