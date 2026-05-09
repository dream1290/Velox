/* vlx_settings_dialog.h — AdwPreferencesDialog for app settings
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define VLX_TYPE_SETTINGS_DIALOG (vlx_settings_dialog_get_type ())
G_DECLARE_FINAL_TYPE (VlxSettingsDialog, vlx_settings_dialog,
                      VLX, SETTINGS_DIALOG, AdwPreferencesDialog)

VlxSettingsDialog *vlx_settings_dialog_new (void);

G_END_DECLS
