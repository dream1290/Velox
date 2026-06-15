/* vlx_thumbnail.h — Async LRU thumbnail cache
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <glib-object.h>
#include <gdk/gdk.h>

G_BEGIN_DECLS

#define VLX_TYPE_THUMBNAIL_CACHE (vlx_thumbnail_cache_get_type ())
G_DECLARE_FINAL_TYPE (VlxThumbnailCache, vlx_thumbnail_cache,
                      VLX, THUMBNAIL_CACHE, GObject)

typedef void (*VlxThumbnailReadyCb) (const gchar *uri,
                                     gint64      position_us,
                                     GdkTexture  *texture,
                                     gpointer     user_data);

VlxThumbnailCache *vlx_thumbnail_cache_get_default (void);

void vlx_thumbnail_cache_request (VlxThumbnailCache  *cache,
                                  const gchar        *uri,
                                  gint64              position_us,
                                  VlxThumbnailReadyCb callback,
                                  gpointer            user_data);


void vlx_thumbnail_cache_shutdown (void);

G_END_DECLS
