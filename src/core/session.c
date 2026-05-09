/* vlx_session.c — Resume-position persistence via GSettings
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Stores resume positions as a GVariant dictionary (a{sx}) in GSettings.
 * Key = URI, Value = position in microseconds.
 */

#include "core/session.h"
#include "utils/log.h"

#include <gio/gio.h>

#define SETTINGS_SCHEMA  "io.github.velox"
#define SETTINGS_KEY     "resume-positions"

struct _VlxSession {
    GObject    parent_instance;
    GSettings *settings;
};

G_DEFINE_TYPE (VlxSession, vlx_session, G_TYPE_OBJECT)

static void
vlx_session_finalize (GObject *obj)
{
    VlxSession *self = VLX_SESSION (obj);
    g_clear_object (&self->settings);
    G_OBJECT_CLASS (vlx_session_parent_class)->finalize (obj);
}

static void
vlx_session_class_init (VlxSessionClass *klass)
{
    G_OBJECT_CLASS (klass)->finalize = vlx_session_finalize;
}

static void
vlx_session_init (VlxSession *self)
{
    GSettingsSchemaSource *src = g_settings_schema_source_get_default ();
    if (src) {
        GSettingsSchema *schema =
            g_settings_schema_source_lookup (src, SETTINGS_SCHEMA, TRUE);
        if (schema) {
            self->settings = g_settings_new (SETTINGS_SCHEMA);
            g_settings_schema_unref (schema);
        }
    }
    if (!self->settings)
        VLX_LOG_WARNING (VLX_LOG_DOMAIN_CORE,
                         "GSettings schema '%s' not found — "
                         "resume positions will not persist",
                         SETTINGS_SCHEMA);
}

VlxSession *
vlx_session_new (void)
{
    return g_object_new (VLX_TYPE_SESSION, NULL);
}

void
vlx_session_save_position (VlxSession  *self,
                           const gchar *uri,
                           gint64       position_us)
{
    g_return_if_fail (VLX_IS_SESSION (self));
    g_return_if_fail (uri != NULL);

    if (!self->settings) return;

    GVariant *current = g_settings_get_value (self->settings, SETTINGS_KEY);
    GVariantBuilder builder;
    g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sx}"));

    /* Copy existing entries, replacing if URI matches */
    GVariantIter iter;
    const gchar *key;
    gint64 val;
    gboolean found = FALSE;

    g_variant_iter_init (&iter, current);
    while (g_variant_iter_next (&iter, "{&sx}", &key, &val)) {
        if (g_strcmp0 (key, uri) == 0) {
            g_variant_builder_add (&builder, "{sx}", key, position_us);
            found = TRUE;
        } else {
            g_variant_builder_add (&builder, "{sx}", key, val);
        }
    }

    if (!found)
        g_variant_builder_add (&builder, "{sx}", uri, position_us);

    g_settings_set_value (self->settings, SETTINGS_KEY,
                          g_variant_builder_end (&builder));
    g_variant_unref (current);
}

gint64
vlx_session_get_position (VlxSession  *self,
                          const gchar *uri)
{
    g_return_val_if_fail (VLX_IS_SESSION (self), 0);
    g_return_val_if_fail (uri != NULL, 0);

    if (!self->settings) return 0;

    GVariant *current = g_settings_get_value (self->settings, SETTINGS_KEY);
    GVariantIter iter;
    const gchar *key;
    gint64 val;

    g_variant_iter_init (&iter, current);
    while (g_variant_iter_next (&iter, "{&sx}", &key, &val)) {
        if (g_strcmp0 (key, uri) == 0) {
            g_variant_unref (current);
            return val;
        }
    }

    g_variant_unref (current);
    return 0;
}

void
vlx_session_clear (VlxSession  *self,
                   const gchar *uri)
{
    g_return_if_fail (VLX_IS_SESSION (self));
    if (!self->settings) return;

    if (!uri) {
        g_settings_reset (self->settings, SETTINGS_KEY);
        return;
    }

    GVariant *current = g_settings_get_value (self->settings, SETTINGS_KEY);
    GVariantBuilder builder;
    g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sx}"));

    GVariantIter iter;
    const gchar *key;
    gint64 val;

    g_variant_iter_init (&iter, current);
    while (g_variant_iter_next (&iter, "{&sx}", &key, &val)) {
        if (g_strcmp0 (key, uri) != 0)
            g_variant_builder_add (&builder, "{sx}", key, val);
    }

    g_settings_set_value (self->settings, SETTINGS_KEY,
                          g_variant_builder_end (&builder));
    g_variant_unref (current);
}
