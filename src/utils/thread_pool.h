/* vlx_thread_pool.h — GThreadPool abstraction for background work
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Provides a shared worker pool for L4 media services (thumbnailing,
 * metadata extraction, subtitle parsing).  Results are marshalled back
 * to the GLib main loop via g_main_context_invoke().
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

/**
 * VlxWorkFunc:
 * @task_data: Data passed to vlx_thread_pool_push().
 *
 * Callback that runs in a worker thread.  Must not touch GTK widgets.
 * Return value is passed to the completion callback on the main thread.
 */
typedef gpointer (*VlxWorkFunc)      (gpointer task_data);

/**
 * VlxCompletionFunc:
 * @result:    Return value from VlxWorkFunc.
 * @user_data: User data passed to vlx_thread_pool_push().
 *
 * Callback that runs on the GLib main loop thread after the work
 * function completes.
 */
typedef void     (*VlxCompletionFunc) (gpointer result,
                                       gpointer user_data);

/**
 * vlx_thread_pool_init:
 * @max_threads: Maximum concurrent worker threads (0 = unlimited).
 *
 * Initialise the global thread pool.  Call once at startup.
 */
void vlx_thread_pool_init (guint max_threads);

/**
 * vlx_thread_pool_push:
 * @work_fn:     Function to run in the worker thread.
 * @complete_fn: (nullable): Callback invoked on the main loop with the result.
 * @task_data:   Data passed to @work_fn.
 * @user_data:   Data passed to @complete_fn.
 * @free_fn:     (nullable): GDestroyNotify for @task_data after completion.
 *
 * Schedule a unit of work.  The work function runs off the main thread;
 * completion is dispatched back to the default GMainContext.
 */
void vlx_thread_pool_push (VlxWorkFunc       work_fn,
                            VlxCompletionFunc complete_fn,
                            gpointer          task_data,
                            gpointer          user_data,
                            GDestroyNotify    free_fn);

/**
 * vlx_thread_pool_shutdown:
 *
 * Wait for outstanding tasks and release the thread pool.
 */
void vlx_thread_pool_shutdown (void);

G_END_DECLS
