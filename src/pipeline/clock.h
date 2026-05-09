/* vlx_clock.h — Custom A/V sync clock
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gst/gst.h>

G_BEGIN_DECLS

#define VLX_TYPE_CLOCK (vlx_clock_get_type ())
G_DECLARE_FINAL_TYPE (VlxClock, vlx_clock, VLX, CLOCK, GstSystemClock)

VlxClock *vlx_clock_new (void);

G_END_DECLS
