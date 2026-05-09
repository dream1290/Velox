/* vlx_pip_window.h — Always-on-top Picture-in-Picture window
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Creates a small, frameless, always-on-top GtkWindow that renders the
 * current video frame from a VlxVideoWidget via a shared GdkTexture.
 * The window follows the main window's display.
 *
 * Wayland note: gtk_window_set_keep_above() is a hint; compositors may
 * honour it via the xdg-shell always_on_top protocol extension.
 */

#pragma once

#include <gtk/gtk.h>
#include "ui/video_widget.h"

G_BEGIN_DECLS

#define VLX_TYPE_PIP_WINDOW (vlx_pip_window_get_type ())
G_DECLARE_FINAL_TYPE (VlxPipWindow, vlx_pip_window, VLX, PIP_WINDOW, GtkWindow)

/* Create a PiP window sharing frames from @source_widget.
 * The window is NOT shown until vlx_pip_window_present() is called. */
VlxPipWindow *vlx_pip_window_new        (VlxVideoWidget *source_widget);

void          vlx_pip_window_present    (VlxPipWindow *self,
                                         GtkWindow    *parent);
void          vlx_pip_window_close_pip  (VlxPipWindow *self);

G_END_DECLS
