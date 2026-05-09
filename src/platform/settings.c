/* vlx_settings.c — Typed GSettings wrapper
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "platform/settings.h"
#include "utils/log.h"

#include <gio/gio.h>

#define SCHEMA "io.github.velox"

struct _VlxSettings {
    GObject    parent_instance;
    GSettings *gs;
};

G_DEFINE_TYPE (VlxSettings, vlx_settings, G_TYPE_OBJECT)

static void
vlx_settings_finalize (GObject *obj)
{
    g_clear_object (&VLX_SETTINGS (obj)->gs);
    G_OBJECT_CLASS (vlx_settings_parent_class)->finalize (obj);
}

static void
vlx_settings_class_init (VlxSettingsClass *klass)
{
    G_OBJECT_CLASS (klass)->finalize = vlx_settings_finalize;
}

static void
vlx_settings_init (VlxSettings *self)
{
    GSettingsSchemaSource *src = g_settings_schema_source_get_default ();
    GSettingsSchema *schema = src
        ? g_settings_schema_source_lookup (src, SCHEMA, TRUE)
        : NULL;

    if (schema) {
        self->gs = g_settings_new (SCHEMA);
        g_settings_schema_unref (schema);
    } else {
        VLX_LOG_WARNING (VLX_LOG_DOMAIN_PLATFORM,
                         "GSettings schema '%s' not installed — "
                         "using in-memory defaults", SCHEMA);
        self->gs = NULL;
    }
}

VlxSettings *
vlx_settings_get_default (void)
{
    static VlxSettings *instance = NULL;
    if (g_once_init_enter_pointer (&instance)) {
        VlxSettings *s = g_object_new (VLX_TYPE_SETTINGS, NULL);
        g_once_init_leave_pointer (&instance, s);
    }
    return instance;
}

/* ── Typed accessors ─────────────────────────────────────────────────── */
gdouble vlx_settings_get_volume (VlxSettings *self)
{
    if (!self->gs) return 1.0;
    return g_settings_get_double (self->gs, "default-volume");
}
void vlx_settings_set_volume (VlxSettings *self, gdouble v)
{
    if (self->gs) g_settings_set_double (self->gs, "default-volume", v);
}

gboolean vlx_settings_get_resume (VlxSettings *self)
{
    if (!self->gs) return TRUE;
    return g_settings_get_boolean (self->gs, "resume-playback");
}
void vlx_settings_set_resume (VlxSettings *self, gboolean v)
{
    if (self->gs) g_settings_set_boolean (self->gs, "resume-playback", v);
}

gboolean vlx_settings_get_hwaccel (VlxSettings *self)
{
    if (!self->gs) return TRUE;
    return g_settings_get_boolean (self->gs, "hardware-acceleration");
}
void vlx_settings_set_hwaccel (VlxSettings *self, gboolean v)
{
    if (self->gs) g_settings_set_boolean (self->gs, "hardware-acceleration", v);
}

gboolean vlx_settings_get_dark_mode (VlxSettings *self)
{
    if (!self->gs) return TRUE;
    return g_settings_get_boolean (self->gs, "dark-mode");
}
void vlx_settings_set_dark_mode (VlxSettings *self, gboolean v)
{
    if (self->gs) g_settings_set_boolean (self->gs, "dark-mode", v);
}

guint vlx_settings_get_osd_timeout_ms (VlxSettings *self)
{
    if (!self->gs) return 2500;
    return (guint) g_settings_get_uint (self->gs, "osd-timeout-ms");
}
void vlx_settings_set_osd_timeout_ms (VlxSettings *self, guint ms)
{
    if (self->gs) g_settings_set_uint (self->gs, "osd-timeout-ms", ms);
}
