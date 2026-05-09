/* vlx_metadata.h — GStreamer tag-list metadata reader
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

typedef struct {
    gchar  *title;
    gchar  *artist;
    gchar  *album;
    gchar  *genre;
    gint    year;
    gint64  duration_us;
    gchar  *codec_video;
    gchar  *codec_audio;
    gint    width;
    gint    height;
    gdouble frame_rate;
} VlxMediaInfo;

void          vlx_media_info_free (VlxMediaInfo *info);

typedef void (*VlxMetadataReadyCb) (const gchar  *uri,
                                    VlxMediaInfo *info,   /* transfer full */
                                    gpointer      user_data);

void vlx_metadata_read_async (const gchar        *uri,
                               VlxMetadataReadyCb  callback,
                               gpointer            user_data);

G_END_DECLS
