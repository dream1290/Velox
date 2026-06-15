/* velox.h — Master include for Velox Video Player
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright 2026 Velox Contributors
 *
 * This header pulls in the public API of every Velox subsystem.
 * Individual modules should include only what they need; this file
 * exists for convenience in main.c and tests.
 */

#pragma once

#include <adwaita.h>
#include <glib.h>
#include <gst/gst.h>
#include <gtk/gtk.h>

/* ── Version info ──────────────────────────────────────────────────────────── */
#ifndef VELOX_VERSION
#define VELOX_VERSION "0.1.0"
#endif

/* ── Application ID ────────────────────────────────────────────────────────── */
#define VLX_APPLICATION_ID "io.github.velox"

/* ── Core (L2) ─────────────────────────────────────────────────────────────── */
#include "core/event_bus.h"
#include "core/player.h"
#include "core/playlist.h"

/* ── Pipeline (L3) ─────────────────────────────────────────────────────────── */
#include "pipeline/hwaccel.h"
#include "pipeline/pipeline.h"

/* ── UI (L1) ───────────────────────────────────────────────────────────────── */
#include "ui/controls_overlay.h"
#include "ui/playlist_panel.h"
#include "ui/seek_bar.h"
#include "ui/video_widget.h"
#include "ui/window.h"

/* ── Media (L4) ────────────────────────────────────────────────────────────── */

#include "media/metadata.h"
#include "media/thumbnail.h"

/* ── Platform (L5) ─────────────────────────────────────────────────────────── */
#include "platform/inhibit.h"
#include "platform/mpris.h"

/* ── Utilities ─────────────────────────────────────────────────────────────── */
#include "utils/cache.h"
#include "utils/log.h"
#include "utils/thread_pool.h"
