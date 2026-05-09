/* vlx_settings.h — Typed GSettings wrapper
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

#define VLX_TYPE_SETTINGS (vlx_settings_get_type ())
G_DECLARE_FINAL_TYPE (VlxSettings, vlx_settings, VLX, SETTINGS, GObject)

VlxSettings *vlx_settings_get_default (void);

gdouble  vlx_settings_get_volume         (VlxSettings *self);
void     vlx_settings_set_volume         (VlxSettings *self, gdouble v);

gboolean vlx_settings_get_resume         (VlxSettings *self);
void     vlx_settings_set_resume         (VlxSettings *self, gboolean v);

gboolean vlx_settings_get_hwaccel        (VlxSettings *self);
void     vlx_settings_set_hwaccel        (VlxSettings *self, gboolean v);

gboolean vlx_settings_get_dark_mode      (VlxSettings *self);
void     vlx_settings_set_dark_mode      (VlxSettings *self, gboolean v);

guint    vlx_settings_get_osd_timeout_ms (VlxSettings *self);
void     vlx_settings_set_osd_timeout_ms (VlxSettings *self, guint ms);

G_END_DECLS
