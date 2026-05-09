/* vlx_discovery.h — GFileMonitor directory watcher
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <glib-object.h>
#include "core/playlist.h"

G_BEGIN_DECLS

#define VLX_TYPE_MEDIA_DISCOVERY (vlx_media_discovery_get_type ())
G_DECLARE_FINAL_TYPE (VlxMediaDiscovery, vlx_media_discovery,
                      VLX, MEDIA_DISCOVERY, GObject)

VlxMediaDiscovery *vlx_media_discovery_new     (VlxPlaylist *playlist);
void               vlx_media_discovery_watch   (VlxMediaDiscovery *self,
                                                const gchar       *directory);
void               vlx_media_discovery_unwatch (VlxMediaDiscovery *self,
                                                const gchar       *directory);

G_END_DECLS
