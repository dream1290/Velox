/* vlx_cache.c — Generic LRU cache implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Implementation notes:
 *   - GQueue holds keys in LRU order (tail = MRU, head = LRU).
 *   - GHashTable maps key → CacheEntry which stores {value, GList* link}.
 *   - On lookup, the link is moved to the tail (MRU promotion).
 *   - On insert at capacity, the head (LRU) is evicted.
 *   - A GMutex protects all mutations for thread safety.
 */

#include "utils/cache.h"

typedef struct {
    gpointer value;
    GList   *link;       /* pointer into the GQueue's internal list */
    gpointer key_copy;   /* kept so we can free the key on eviction */
} CacheEntry;

struct _VlxCache {
    GHashTable     *map;
    GQueue         *order;     /* head = LRU, tail = MRU */
    guint           capacity;
    GMutex          lock;
    GDestroyNotify  key_free;
    GDestroyNotify  value_free;
};

/* ── Helpers ───────────────────────────────────────────────────────────────── */
static void
cache_entry_free (VlxCache *cache, CacheEntry *entry)
{
    if (cache->value_free && entry->value)
        cache->value_free (entry->value);
    if (cache->key_free && entry->key_copy)
        cache->key_free (entry->key_copy);
    g_slice_free (CacheEntry, entry);
}

static void
evict_lru (VlxCache *cache)
{
    gpointer lru_key = g_queue_pop_head (cache->order);
    if (!lru_key)
        return;

    CacheEntry *entry = g_hash_table_lookup (cache->map, lru_key);
    if (entry) {
        /* Steal so the hash table doesn't call its own free func */
        g_hash_table_steal (cache->map, lru_key);
        cache_entry_free (cache, entry);
    }
}

/* ── Public API ────────────────────────────────────────────────────────────── */
VlxCache *
vlx_cache_new (guint          capacity,
               GDestroyNotify key_free,
               GDestroyNotify value_free)
{
    g_return_val_if_fail (capacity > 0, NULL);

    VlxCache *cache = g_new0 (VlxCache, 1);
    cache->capacity   = capacity;
    cache->key_free   = key_free;
    cache->value_free = value_free;

    /* Keys are compared by string — fits URI+mtime hash keys.
     * The hash table does NOT own entries; we manage lifetime manually. */
    cache->map   = g_hash_table_new (g_str_hash, g_str_equal);
    cache->order = g_queue_new ();
    g_mutex_init (&cache->lock);

    return cache;
}

void
vlx_cache_free (VlxCache *cache)
{
    if (!cache)
        return;

    vlx_cache_clear (cache);
    g_hash_table_destroy (cache->map);
    g_queue_free (cache->order);
    g_mutex_clear (&cache->lock);
    g_free (cache);
}

void
vlx_cache_insert (VlxCache *cache,
                  gpointer  key,
                  gpointer  value)
{
    g_return_if_fail (cache != NULL);
    g_return_if_fail (key != NULL);

    g_mutex_lock (&cache->lock);

    /* If key already exists, update in place */
    CacheEntry *existing = g_hash_table_lookup (cache->map, key);
    if (existing) {
        if (cache->value_free && existing->value)
            cache->value_free (existing->value);
        existing->value = value;
        /* Promote to MRU */
        g_queue_unlink (cache->order, existing->link);
        g_queue_push_tail_link (cache->order, existing->link);
        /* Free the duplicate key since we already have one */
        if (cache->key_free)
            cache->key_free (key);
        g_mutex_unlock (&cache->lock);
        return;
    }

    /* Evict if at capacity */
    while (g_hash_table_size (cache->map) >= cache->capacity)
        evict_lru (cache);

    /* Insert new entry */
    CacheEntry *entry = g_slice_new0 (CacheEntry);
    entry->value    = value;
    entry->key_copy = key;

    g_queue_push_tail (cache->order, key);
    entry->link = g_queue_peek_tail_link (cache->order);

    g_hash_table_insert (cache->map, key, entry);

    g_mutex_unlock (&cache->lock);
}

gpointer
vlx_cache_lookup (VlxCache      *cache,
                  gconstpointer  key)
{
    g_return_val_if_fail (cache != NULL, NULL);

    g_mutex_lock (&cache->lock);

    CacheEntry *entry = g_hash_table_lookup (cache->map, key);
    if (!entry) {
        g_mutex_unlock (&cache->lock);
        return NULL;
    }

    /* Promote to MRU */
    g_queue_unlink (cache->order, entry->link);
    g_queue_push_tail_link (cache->order, entry->link);

    gpointer val = entry->value;
    g_mutex_unlock (&cache->lock);
    return val;
}

gboolean
vlx_cache_remove (VlxCache      *cache,
                  gconstpointer  key)
{
    g_return_val_if_fail (cache != NULL, FALSE);

    g_mutex_lock (&cache->lock);

    CacheEntry *entry = g_hash_table_lookup (cache->map, key);
    if (!entry) {
        g_mutex_unlock (&cache->lock);
        return FALSE;
    }

    g_queue_unlink (cache->order, entry->link);
    g_queue_delete_link (cache->order, entry->link);
    g_hash_table_steal (cache->map, key);
    cache_entry_free (cache, entry);

    g_mutex_unlock (&cache->lock);
    return TRUE;
}

void
vlx_cache_clear (VlxCache *cache)
{
    g_return_if_fail (cache != NULL);

    g_mutex_lock (&cache->lock);

    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init (&iter, cache->map);
    while (g_hash_table_iter_next (&iter, &key, &value)) {
        CacheEntry *entry = value;
        cache_entry_free (cache, entry);
        g_hash_table_iter_steal (&iter);
    }

    g_queue_clear (cache->order);

    g_mutex_unlock (&cache->lock);
}

guint
vlx_cache_size (VlxCache *cache)
{
    g_return_val_if_fail (cache != NULL, 0);

    g_mutex_lock (&cache->lock);
    guint size = g_hash_table_size (cache->map);
    g_mutex_unlock (&cache->lock);
    return size;
}
