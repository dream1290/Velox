/* vlx_session.h — Resume-position persistence via GSettings
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

#define VLX_TYPE_SESSION (vlx_session_get_type ())
G_DECLARE_FINAL_TYPE (VlxSession, vlx_session, VLX, SESSION, GObject)

VlxSession *vlx_session_new          (void);

void        vlx_session_save_position (VlxSession  *self,
                                       const gchar *uri,
                                       gint64       position_us);

gint64      vlx_session_get_position  (VlxSession  *self,
                                       const gchar *uri);

void        vlx_session_clear         (VlxSession  *self,
                                       const gchar *uri);

G_END_DECLS
