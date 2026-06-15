project('velox', 'c',
  version : '3.0.2',
  license : 'GPL-3.0-or-later',
  meson_version : '>= 0.62.0',
  default_options : [
    'c_std=gnu17',
    'warning_level=2',
    'buildtype=debugoptimized',
  ],
)

# ── Dependencies ─────────────────────────────────────────────────────────────
gtk4        = dependency('gtk4',           version : '>= 4.12')
adw         = dependency('libadwaita-1',   version : '>= 1.4')
gstreamer   = dependency('gstreamer-1.0',  version : '>= 1.22')
gst_video   = dependency('gstreamer-video-1.0')
gst_audio   = dependency('gstreamer-audio-1.0')
gst_pbutils = dependency('gstreamer-pbutils-1.0')
gst_gl      = dependency('gstreamer-gl-1.0', required : false)
gio         = dependency('gio-2.0')
glib        = dependency('glib-2.0',      version : '>= 2.74')
gobject     = dependency('gobject-2.0')
cairo       = dependency('cairo')
pango       = dependency('pango')
pangocairo  = dependency('pangocairo')
epoxy       = dependency('epoxy')
sqlite3     = dependency('sqlite3')
math        = meson.get_compiler('c').find_library('m', required : false)

velox_deps = [
  gtk4,
  adw,
  gstreamer,
  gst_video,
  gst_audio,
  gst_pbutils,
  gst_gl,
  gio,
  glib,
  gobject,
  cairo,
  pango,
  pangocairo,
  epoxy,
  sqlite3,
  math,
]

# ── Compile flags ────────────────────────────────────────────────────────────
add_project_arguments([
  '-DVELOX_VERSION="@0@"'.format(meson.project_version()),
  '-DG_LOG_DOMAIN="Velox"',
  '-DGETTEXT_PACKAGE="velox"',
  '-DVELOX_DATADIR="@0@"'.format(join_paths(get_option('prefix'), get_option('datadir'))),
  '-Wno-unused-parameter',
], language : 'c')

# ── Source files ─────────────────────────────────────────────────────────────
velox_sources = files(
  'src/main.c',

  # Core (L2 — Application)
  'src/core/player.c',
  'src/core/event_bus.c',
  'src/core/playlist.c',

  # Pipeline (L3 — Media Engine)
  'src/pipeline/pipeline.c',
  'src/pipeline/hwaccel.c',

  # UI (L1 — Presentation)
  'src/ui/window.c',
  'src/ui/video_widget.c',
  'src/ui/controls_overlay.c',
  'src/ui/seek_bar.c',
  'src/ui/playlist_panel.c',
  'src/ui/pip_window.c',

  # Media (L4 — Media Services)
  'src/media/thumbnail.c',
  'src/media/metadata.c',
  'src/media/history.c',

  # Platform (L5 — OS Integration)
  'src/platform/mpris.c',
  'src/platform/inhibit.c',

  # Utilities
  'src/utils/thread_pool.c',
  'src/utils/cache.c',
  'src/utils/log.c',
)

velox_inc = include_directories('src')

# ── Executable ───────────────────────────────────────────────────────────────
executable('velox',
  velox_sources,
  dependencies    : velox_deps,
  include_directories : velox_inc,
  install         : true,
  install_dir     : get_option('bindir'),
)




# ── Tests (stub) ─────────────────────────────────────────────────────────────
# subdir('tests')