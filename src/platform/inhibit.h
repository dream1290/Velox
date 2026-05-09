/* vlx_inhibit.h — System sleep inhibitor
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

#define VLX_TYPE_INHIBIT_MANAGER (vlx_inhibit_manager_get_type ())
G_DECLARE_FINAL_TYPE (VlxInhibitManager, vlx_inhibit_manager,
                      VLX, INHIBIT_MANAGER, GObject)

VlxInhibitManager *vlx_inhibit_manager_new     (void);
void               vlx_inhibit_manager_inhibit  (VlxInhibitManager *self);
void               vlx_inhibit_manager_uninhibit(VlxInhibitManager *self);

G_END_DECLS
