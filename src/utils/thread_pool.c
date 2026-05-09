/* vlx_thread_pool.c — GThreadPool wrapper
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "utils/thread_pool.h"
#include "utils/log.h"

/* ── Internal task descriptor ──────────────────────────────────────────────── */
typedef struct {
    VlxWorkFunc       work_fn;
    VlxCompletionFunc complete_fn;
    gpointer          task_data;
    gpointer          user_data;
    GDestroyNotify    free_fn;
    gpointer          result;
} TaskDesc;

static GThreadPool *g_pool = NULL;

/* ── Main-loop trampoline ──────────────────────────────────────────────────── */
static gboolean
dispatch_completion (gpointer data)
{
    TaskDesc *td = data;

    if (td->complete_fn)
        td->complete_fn (td->result, td->user_data);

    if (td->free_fn && td->task_data)
        td->free_fn (td->task_data);

    g_slice_free (TaskDesc, td);
    return G_SOURCE_REMOVE;
}

/* ── Worker thread entry ───────────────────────────────────────────────────── */
static void
worker_func (gpointer data, gpointer user_data)
{
    (void) user_data;
    TaskDesc *td = data;

    td->result = td->work_fn (td->task_data);

    /* Post result back to the main loop */
    g_main_context_invoke (NULL, dispatch_completion, td);
}

/* ── Public API ────────────────────────────────────────────────────────────── */
void
vlx_thread_pool_init (guint max_threads)
{
    GError *err = NULL;

    if (max_threads == 0)
        max_threads = (guint) g_get_num_processors ();

    g_pool = g_thread_pool_new (worker_func,
                                NULL,
                                (gint) max_threads,
                                FALSE, /* non-exclusive */
                                &err);
    if (err) {
        VLX_LOG_ERROR (VLX_LOG_DOMAIN_CORE,
                       "Failed to create thread pool: %s", err->message);
        g_error_free (err);
        return;
    }

    VLX_LOG_INFO (VLX_LOG_DOMAIN_CORE,
                  "Thread pool initialised (max_threads=%u)", max_threads);
}

void
vlx_thread_pool_push (VlxWorkFunc       work_fn,
                      VlxCompletionFunc complete_fn,
                      gpointer          task_data,
                      gpointer          user_data,
                      GDestroyNotify    free_fn)
{
    g_return_if_fail (g_pool != NULL);
    g_return_if_fail (work_fn != NULL);

    TaskDesc *td    = g_slice_new0 (TaskDesc);
    td->work_fn     = work_fn;
    td->complete_fn = complete_fn;
    td->task_data   = task_data;
    td->user_data   = user_data;
    td->free_fn     = free_fn;

    GError *err = NULL;
    g_thread_pool_push (g_pool, td, &err);
    if (err) {
        VLX_LOG_ERROR (VLX_LOG_DOMAIN_CORE,
                       "Failed to push task to pool: %s", err->message);
        g_error_free (err);
        g_slice_free (TaskDesc, td);
    }
}

void
vlx_thread_pool_shutdown (void)
{
    if (!g_pool)
        return;

    VLX_LOG_INFO (VLX_LOG_DOMAIN_CORE, "Shutting down thread pool...");
    g_thread_pool_free (g_pool, FALSE, TRUE);
    g_pool = NULL;
}
