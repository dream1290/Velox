/* vlx_log.h — Structured logging for Velox
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Wraps GLib structured logging with domain-specific convenience macros.
 * All log output is channeled through g_log_structured() so it integrates
 * with journalctl and GNOME Logs.
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* ── Log domains ───────────────────────────────────────────────────────────── */
#define VLX_LOG_DOMAIN_CORE     "Velox-Core"
#define VLX_LOG_DOMAIN_PIPELINE "Velox-Pipeline"
#define VLX_LOG_DOMAIN_UI       "Velox-UI"
#define VLX_LOG_DOMAIN_MEDIA    "Velox-Media"
#define VLX_LOG_DOMAIN_PLATFORM "Velox-Platform"

/* ── Convenience macros ────────────────────────────────────────────────────── */
#define VLX_LOG_DEBUG(domain, fmt, ...) \
    g_log_structured (domain, G_LOG_LEVEL_DEBUG,    \
                      "CODE_FILE", __FILE__,        \
                      "CODE_LINE", G_STRINGIFY(__LINE__), \
                      "CODE_FUNC", G_STRFUNC,       \
                      "MESSAGE", fmt, ##__VA_ARGS__)

#define VLX_LOG_INFO(domain, fmt, ...) \
    g_log_structured (domain, G_LOG_LEVEL_INFO,     \
                      "CODE_FILE", __FILE__,        \
                      "CODE_LINE", G_STRINGIFY(__LINE__), \
                      "CODE_FUNC", G_STRFUNC,       \
                      "MESSAGE", fmt, ##__VA_ARGS__)

#define VLX_LOG_MESSAGE(domain, fmt, ...) \
    g_log_structured (domain, G_LOG_LEVEL_MESSAGE,  \
                      "CODE_FILE", __FILE__,        \
                      "CODE_LINE", G_STRINGIFY(__LINE__), \
                      "CODE_FUNC", G_STRFUNC,       \
                      "MESSAGE", fmt, ##__VA_ARGS__)

#define VLX_LOG_WARNING(domain, fmt, ...) \
    g_log_structured (domain, G_LOG_LEVEL_WARNING,  \
                      "CODE_FILE", __FILE__,        \
                      "CODE_LINE", G_STRINGIFY(__LINE__), \
                      "CODE_FUNC", G_STRFUNC,       \
                      "MESSAGE", fmt, ##__VA_ARGS__)

#define VLX_LOG_ERROR(domain, fmt, ...) \
    g_log_structured (domain, G_LOG_LEVEL_CRITICAL, \
                      "CODE_FILE", __FILE__,        \
                      "CODE_LINE", G_STRINGIFY(__LINE__), \
                      "CODE_FUNC", G_STRFUNC,       \
                      "MESSAGE", fmt, ##__VA_ARGS__)

/* ── Initialization ────────────────────────────────────────────────────────── */

/**
 * vlx_log_init:
 *
 * Install the Velox log writer that colourises output for interactive
 * terminals and emits structured fields for journald.  Call once at
 * startup before any other Velox API.
 */
void vlx_log_init (void);

G_END_DECLS
