/* vlx_log.c — Structured logging implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "utils/log.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* ── ANSI colour codes ─────────────────────────────────────────────────────── */
#define CLR_RESET   "\033[0m"
#define CLR_DIM     "\033[2m"
#define CLR_RED     "\033[1;31m"
#define CLR_YELLOW  "\033[1;33m"
#define CLR_CYAN    "\033[0;36m"
#define CLR_GREEN   "\033[0;32m"
#define CLR_MAGENTA "\033[0;35m"

static gboolean use_colour = FALSE;

/* ── Custom log writer ─────────────────────────────────────────────────────── */
static GLogWriterOutput
vlx_log_writer (GLogLevelFlags   log_level,
                const GLogField *fields,
                gsize            n_fields,
                gpointer         user_data)
{
    const gchar *message  = NULL;
    const gchar *domain   = NULL;
    const gchar *file     = NULL;
    const gchar *line     = NULL;

    for (gsize i = 0; i < n_fields; i++) {
        if (g_strcmp0 (fields[i].key, "MESSAGE") == 0)
            message = (const gchar *) fields[i].value;
        else if (g_strcmp0 (fields[i].key, "GLIB_DOMAIN") == 0)
            domain = (const gchar *) fields[i].value;
        else if (g_strcmp0 (fields[i].key, "CODE_FILE") == 0)
            file = (const gchar *) fields[i].value;
        else if (g_strcmp0 (fields[i].key, "CODE_LINE") == 0)
            line = (const gchar *) fields[i].value;
    }

    if (!message)
        message = "(no message)";
    if (!domain)
        domain = "Velox";

    /* Respect G_MESSAGES_DEBUG for debug and info messages */
    if (g_log_writer_default_would_drop (log_level, domain))
        return G_LOG_WRITER_HANDLED;

    const gchar *level_str;
    const gchar *colour;

    switch (log_level & G_LOG_LEVEL_MASK) {
    case G_LOG_LEVEL_ERROR:
    case G_LOG_LEVEL_CRITICAL:
        level_str = "ERR";
        colour    = CLR_RED;
        break;
    case G_LOG_LEVEL_WARNING:
        level_str = "WRN";
        colour    = CLR_YELLOW;
        break;
    case G_LOG_LEVEL_MESSAGE:
    case G_LOG_LEVEL_INFO:
        level_str = "INF";
        colour    = CLR_GREEN;
        break;
    case G_LOG_LEVEL_DEBUG:
        level_str = "DBG";
        colour    = CLR_CYAN;
        break;
    default:
        level_str = "???";
        colour    = CLR_RESET;
        break;
    }

    if (use_colour) {
        fprintf (stderr, "%s[%s]%s %s%-16s%s %s",
                 colour, level_str, CLR_RESET,
                 CLR_MAGENTA, domain, CLR_RESET,
                 message);
        if (file && line)
            fprintf (stderr, "  %s(%s:%s)%s", CLR_DIM, file, line, CLR_RESET);
        fputc ('\n', stderr);
    } else {
        fprintf (stderr, "[%s] %-16s %s", level_str, domain, message);
        if (file && line)
            fprintf (stderr, "  (%s:%s)", file, line);
        fputc ('\n', stderr);
    }

    /* Also forward to journald when available */
    return g_log_writer_default (log_level, fields, n_fields, user_data);
}

/* ── Public API ────────────────────────────────────────────────────────────── */
void
vlx_log_init (void)
{
    use_colour = isatty (STDERR_FILENO);
    g_log_set_writer_func (vlx_log_writer, NULL, NULL);

    VLX_LOG_INFO (VLX_LOG_DOMAIN_CORE,
                  "Velox logging initialised (colour=%s)",
                  use_colour ? "yes" : "no");
}
