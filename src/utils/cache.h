/* vlx_cache.h — Generic LRU cache (GHashTable + GQueue)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Thread-safe LRU cache with configurable capacity and custom
 * free functions.  Intended for thumbnail bitmaps, parsed metadata,
 * and subtitle caches.
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct _VlxCache VlxCache;

/**
 * vlx_cache_new:
 * @capacity:  Maximum number of entries.
 * @key_free:  GDestroyNotify for keys (e.g. g_free).
 * @value_free: GDestroyNotify for values.
 *
 * Returns: A new VlxCache.  Free with vlx_cache_free().
 */
VlxCache *vlx_cache_new (guint          capacity,
                          GDestroyNotify key_free,
                          GDestroyNotify value_free);

void      vlx_cache_free (VlxCache *cache);

/**
 * vlx_cache_insert:
 * @cache: the cache
 * @key:   ownership transferred to cache
 * @value: ownership transferred to cache
 *
 * Insert an entry, evicting the LRU entry if at capacity.
 */
void      vlx_cache_insert (VlxCache *cache,
                             gpointer  key,
                             gpointer  value);

/**
 * vlx_cache_lookup:
 * @cache: the cache
 * @key:   lookup key (not consumed)
 *
 * Returns: (transfer none)(nullable): The cached value, or %NULL.
 *          Accessing a value promotes it to MRU.
 */
gpointer  vlx_cache_lookup (VlxCache    *cache,
                             gconstpointer key);

/**
 * vlx_cache_remove:
 * @cache: the cache
 * @key:   the key to evict
 *
 * Returns: TRUE if the entry existed and was removed.
 */
gboolean  vlx_cache_remove (VlxCache    *cache,
                             gconstpointer key);

void      vlx_cache_clear  (VlxCache *cache);
guint     vlx_cache_size   (VlxCache *cache);

G_END_DECLS
