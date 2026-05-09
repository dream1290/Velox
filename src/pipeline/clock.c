/* vlx_clock.c — Custom A/V sync clock
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Subclasses GstSystemClock to provide a stable monotonic clock
 * for the pipeline.  Currently a thin wrapper — the real value is
 * the extension point for future drift correction.
 */

#include "pipeline/clock.h"

struct _VlxClock {
    GstSystemClock parent_instance;
};

G_DEFINE_TYPE (VlxClock, vlx_clock, GST_TYPE_SYSTEM_CLOCK)

static void
vlx_clock_class_init (VlxClockClass *klass)
{
    (void) klass;
}

static void
vlx_clock_init (VlxClock *self)
{
    /* Use monotonic time for stability */
    g_object_set (self, "clock-type", GST_CLOCK_TYPE_MONOTONIC, NULL);
}

VlxClock *
vlx_clock_new (void)
{
    return g_object_new (VLX_TYPE_CLOCK, NULL);
}
