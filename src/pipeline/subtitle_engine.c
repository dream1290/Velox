/* vlx_subtitle_engine.c — SRT/VTT/ASS subtitle parser
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "pipeline/subtitle_engine.h"
#include "utils/log.h"

#include <stdio.h>
#include <string.h>

struct _VlxSubtitleEngine {
    GObject parent_instance;

    GArray *spans;   /* VlxSubtitleSpan, sorted by start_us */
};

G_DEFINE_TYPE (VlxSubtitleEngine, vlx_subtitle_engine, G_TYPE_OBJECT)

static void
span_clear (gpointer data)
{
    VlxSubtitleSpan *span = data;
    g_free (span->text);
    span->text = NULL;
}

static void
vlx_subtitle_engine_finalize (GObject *obj)
{
    VlxSubtitleEngine *self = VLX_SUBTITLE_ENGINE (obj);
    g_array_unref (self->spans);
    G_OBJECT_CLASS (vlx_subtitle_engine_parent_class)->finalize (obj);
}

static void
vlx_subtitle_engine_class_init (VlxSubtitleEngineClass *klass)
{
    G_OBJECT_CLASS (klass)->finalize = vlx_subtitle_engine_finalize;
}

static void
vlx_subtitle_engine_init (VlxSubtitleEngine *self)
{
    self->spans = g_array_new (FALSE, TRUE, sizeof (VlxSubtitleSpan));
    g_array_set_clear_func (self->spans, span_clear);
}

VlxSubtitleEngine *
vlx_subtitle_engine_new (void)
{
    return g_object_new (VLX_TYPE_SUBTITLE_ENGINE, NULL);
}

/* Parse SRT timestamp: HH:MM:SS,mmm → microseconds */
static gint64
parse_srt_timestamp (const gchar *ts)
{
    int h, m, s, ms;
    if (sscanf (ts, "%d:%d:%d,%d", &h, &m, &s, &ms) != 4) {
        /* Try VTT format with . instead of , */
        if (sscanf (ts, "%d:%d:%d.%d", &h, &m, &s, &ms) != 4)
            return -1;
    }
    return ((gint64) h * 3600 + m * 60 + s) * G_USEC_PER_SEC +
           (gint64) ms * 1000;
}

gboolean
vlx_subtitle_engine_load_file (VlxSubtitleEngine *self,
                               const gchar       *path)
{
    g_return_val_if_fail (VLX_IS_SUBTITLE_ENGINE (self), FALSE);
    g_return_val_if_fail (path != NULL, FALSE);

    vlx_subtitle_engine_clear (self);

    gchar *contents = NULL;
    gsize length = 0;
    GError *err = NULL;

    if (!g_file_get_contents (path, &contents, &length, &err)) {
        VLX_LOG_WARNING (VLX_LOG_DOMAIN_PIPELINE,
                         "Failed to read subtitle file: %s", err->message);
        g_error_free (err);
        return FALSE;
    }

    /* Simple SRT parser */
    gchar **lines = g_strsplit (contents, "\n", -1);
    g_free (contents);

    enum { EXPECT_INDEX, EXPECT_TIME, EXPECT_TEXT } state = EXPECT_INDEX;
    VlxSubtitleSpan current = { 0 };
    GString *text_buf = g_string_new (NULL);

    for (gchar **line = lines; *line; line++) {
        g_strstrip (*line);

        switch (state) {
        case EXPECT_INDEX:
            if (strlen (*line) > 0 && g_ascii_isdigit ((*line)[0]))
                state = EXPECT_TIME;
            break;

        case EXPECT_TIME: {
            gchar **parts = g_strsplit (*line, " --> ", 2);
            if (parts[0] && parts[1]) {
                current.start_us = parse_srt_timestamp (parts[0]);
                current.end_us   = parse_srt_timestamp (parts[1]);
                state = EXPECT_TEXT;
                g_string_truncate (text_buf, 0);
            }
            g_strfreev (parts);
            break;
        }
        case EXPECT_TEXT:
            if (strlen (*line) == 0) {
                /* End of subtitle block */
                current.text = g_string_free (g_string_new (text_buf->str),
                                              FALSE);
                g_array_append_val (self->spans, current);
                memset (&current, 0, sizeof (current));
                state = EXPECT_INDEX;
            } else {
                if (text_buf->len > 0)
                    g_string_append_c (text_buf, '\n');
                g_string_append (text_buf, *line);
            }
            break;
        }
    }

    /* Handle last subtitle if file doesn't end with blank line */
    if (state == EXPECT_TEXT && text_buf->len > 0) {
        current.text = g_strdup (text_buf->str);
        g_array_append_val (self->spans, current);
    }

    g_string_free (text_buf, TRUE);
    g_strfreev (lines);

    VLX_LOG_INFO (VLX_LOG_DOMAIN_PIPELINE,
                  "Loaded %u subtitle spans from %s",
                  self->spans->len, path);
    return TRUE;
}

const VlxSubtitleSpan *
vlx_subtitle_engine_get_span_at (VlxSubtitleEngine *self,
                                 gint64             position_us)
{
    g_return_val_if_fail (VLX_IS_SUBTITLE_ENGINE (self), NULL);

    /* Binary search for the active span */
    guint lo = 0, hi = self->spans->len;
    while (lo < hi) {
        guint mid = (lo + hi) / 2;
        VlxSubtitleSpan *s = &g_array_index (self->spans,
                                              VlxSubtitleSpan, mid);
        if (s->end_us <= position_us)
            lo = mid + 1;
        else if (s->start_us > position_us)
            hi = mid;
        else
            return s;
    }
    return NULL;
}

void
vlx_subtitle_engine_clear (VlxSubtitleEngine *self)
{
    g_return_if_fail (VLX_IS_SUBTITLE_ENGINE (self));
    g_array_set_size (self->spans, 0);
}
