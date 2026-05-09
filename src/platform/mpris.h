/* vlx_mpris.h — MPRIS2 D-Bus provider
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <glib-object.h>
#include "core/player.h"

G_BEGIN_DECLS

#define VLX_TYPE_MPRIS_PROVIDER (vlx_mpris_provider_get_type ())
G_DECLARE_FINAL_TYPE (VlxMprisProvider, vlx_mpris_provider,
                      VLX, MPRIS_PROVIDER, GObject)

VlxMprisProvider *vlx_mpris_provider_new   (VlxPlayer *player);
void              vlx_mpris_provider_start (VlxMprisProvider *self);
void              vlx_mpris_provider_stop  (VlxMprisProvider *self);

G_END_DECLS
