/* vlx_history.c — SQLite-backed watch history
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "media/history.h"
#include "utils/log.h"

#include <sqlite3.h>
#include <gio/gio.h>

#define VLX_LOG_DOMAIN_HISTORY "Velox-History"

/* ── Schema ──────────────────────────────────────────────────────────────── */
#define CREATE_TABLE_SQL \
    "CREATE TABLE IF NOT EXISTS history (" \
    "  uri          TEXT PRIMARY KEY NOT NULL," \
    "  title        TEXT," \
    "  last_pos_us  INTEGER NOT NULL DEFAULT 0," \
    "  duration_us  INTEGER NOT NULL DEFAULT 0," \
    "  play_count   INTEGER NOT NULL DEFAULT 0," \
    "  last_watched TEXT NOT NULL DEFAULT (datetime('now'))" \
    ");"

struct _VlxHistory {
    GObject   parent_instance;
    sqlite3  *db;
};

G_DEFINE_TYPE (VlxHistory, vlx_history, G_TYPE_OBJECT)

/* ── Internal helpers ────────────────────────────────────────────────────── */
static gboolean
exec_simple (VlxHistory *self, const gchar *sql)
{
    char *errmsg = NULL;
    if (sqlite3_exec (self->db, sql, NULL, NULL, &errmsg) != SQLITE_OK) {
        VLX_LOG_ERROR (VLX_LOG_DOMAIN_HISTORY, "SQL error: %s", errmsg);
        sqlite3_free (errmsg);
        return FALSE;
    }
    return TRUE;
}

/* ── GObject boilerplate ─────────────────────────────────────────────────── */
static void
vlx_history_finalize (GObject *obj)
{
    VlxHistory *self = VLX_HISTORY (obj);
    if (self->db) {
        sqlite3_close (self->db);
        self->db = NULL;
    }
    G_OBJECT_CLASS (vlx_history_parent_class)->finalize (obj);
}

static void
vlx_history_class_init (VlxHistoryClass *klass)
{
    G_OBJECT_CLASS (klass)->finalize = vlx_history_finalize;
}

static void
vlx_history_init (VlxHistory *self)
{
    /* Open (or create) the database */
    gchar *data_dir = g_build_filename (g_get_user_data_dir (), "velox", NULL);
    g_mkdir_with_parents (data_dir, 0700);

    gchar *db_path = g_build_filename (data_dir, "history.db", NULL);
    g_free (data_dir);

    int rc = sqlite3_open (db_path, &self->db);
    g_free (db_path);

    if (rc != SQLITE_OK) {
        VLX_LOG_ERROR (VLX_LOG_DOMAIN_HISTORY,
                       "Cannot open history DB: %s", sqlite3_errmsg (self->db));
        sqlite3_close (self->db);
        self->db = NULL;
        return;
    }

    /* Enable WAL for better concurrency */
    exec_simple (self, "PRAGMA journal_mode=WAL;");
    exec_simple (self, "PRAGMA synchronous=NORMAL;");
    exec_simple (self, CREATE_TABLE_SQL);

    VLX_LOG_INFO (VLX_LOG_DOMAIN_HISTORY, "Watch history database opened");
}

/* ── Singleton ───────────────────────────────────────────────────────────── */
VlxHistory *
vlx_history_get_default (void)
{
    static VlxHistory *instance = NULL;
    if (g_once_init_enter_pointer (&instance)) {
        VlxHistory *h = g_object_new (VLX_TYPE_HISTORY, NULL);
        g_once_init_leave_pointer (&instance, h);
    }
    return instance;
}

/* ── Public API ──────────────────────────────────────────────────────────── */
void
vlx_history_record (VlxHistory  *self,
                    const gchar *uri,
                    const gchar *title,
                    gint64       duration_us)
{
    g_return_if_fail (VLX_IS_HISTORY (self));
    g_return_if_fail (uri != NULL);
    if (!self->db) return;

    const gchar *sql =
        "INSERT INTO history (uri, title, duration_us, play_count, last_watched)"
        "  VALUES (?1, ?2, ?3, 1, datetime('now'))"
        "  ON CONFLICT(uri) DO UPDATE SET"
        "    title        = excluded.title,"
        "    duration_us  = excluded.duration_us,"
        "    play_count   = play_count + 1,"
        "    last_watched = datetime('now');";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2 (self->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return;

    sqlite3_bind_text  (stmt, 1, uri,   -1, SQLITE_STATIC);
    sqlite3_bind_text  (stmt, 2, title ? title : "", -1, SQLITE_STATIC);
    sqlite3_bind_int64 (stmt, 3, duration_us);

    sqlite3_step (stmt);
    sqlite3_finalize (stmt);

    VLX_LOG_DEBUG (VLX_LOG_DOMAIN_HISTORY, "Recorded: %s", uri);
}

void
vlx_history_update_position (VlxHistory  *self,
                              const gchar *uri,
                              gint64       position_us)
{
    g_return_if_fail (VLX_IS_HISTORY (self));
    if (!self->db || !uri) return;

    const gchar *sql =
        "UPDATE history SET last_pos_us = ?1 WHERE uri = ?2;";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2 (self->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return;

    sqlite3_bind_int64 (stmt, 1, position_us);
    sqlite3_bind_text  (stmt, 2, uri, -1, SQLITE_STATIC);
    sqlite3_step (stmt);
    sqlite3_finalize (stmt);
}

gint64
vlx_history_get_resume_pos (VlxHistory  *self,
                             const gchar *uri)
{
    g_return_val_if_fail (VLX_IS_HISTORY (self), 0);
    if (!self->db || !uri) return 0;

    const gchar *sql =
        "SELECT last_pos_us, duration_us FROM history WHERE uri = ?1;";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2 (self->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return 0;

    sqlite3_bind_text (stmt, 1, uri, -1, SQLITE_STATIC);

    gint64 pos = 0;
    if (sqlite3_step (stmt) == SQLITE_ROW) {
        gint64 last_pos  = sqlite3_column_int64 (stmt, 0);
        gint64 duration  = sqlite3_column_int64 (stmt, 1);

        /* Only resume if: position is > 5% and < 95% of duration */
        if (duration > 0) {
            gdouble frac = (gdouble) last_pos / (gdouble) duration;
            if (frac >= 0.05 && frac <= 0.95)
                pos = last_pos;
        }
    }

    sqlite3_finalize (stmt);
    return pos;
}

GPtrArray *
vlx_history_list (VlxHistory *self, guint max_entries)
{
    GPtrArray *arr = g_ptr_array_new_with_free_func (
        (GDestroyNotify) vlx_history_entry_free);

    if (!self->db) return arr;

    const gchar *sql =
        "SELECT uri, title, last_pos_us, duration_us, play_count, last_watched"
        "  FROM history ORDER BY last_watched DESC LIMIT ?1;";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2 (self->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return arr;

    sqlite3_bind_int (stmt, 1, (int) max_entries);

    while (sqlite3_step (stmt) == SQLITE_ROW) {
        VlxHistoryEntry *e = g_new0 (VlxHistoryEntry, 1);
        e->uri          = g_strdup ((const gchar *) sqlite3_column_text (stmt, 0));
        e->title        = g_strdup ((const gchar *) sqlite3_column_text (stmt, 1));
        e->last_pos_us  = sqlite3_column_int64 (stmt, 2);
        e->duration_us  = sqlite3_column_int64 (stmt, 3);
        e->play_count   = sqlite3_column_int   (stmt, 4);
        e->last_watched = g_strdup ((const gchar *) sqlite3_column_text (stmt, 5));
        g_ptr_array_add (arr, e);
    }

    sqlite3_finalize (stmt);
    return arr;
}

void
vlx_history_entry_free (VlxHistoryEntry *entry)
{
    if (!entry) return;
    g_free (entry->uri);
    g_free (entry->title);
    g_free (entry->last_watched);
    g_free (entry);
}

void
vlx_history_clear_all (VlxHistory *self)
{
    g_return_if_fail (VLX_IS_HISTORY (self));
    if (!self->db) return;
    exec_simple (self, "DELETE FROM history;");
}

