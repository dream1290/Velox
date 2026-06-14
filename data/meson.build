install_data(
  'io.github.velox.gschema.xml',
  install_dir: join_paths(get_option('datadir'), 'glib-2.0', 'schemas')
)

install_data(
  'io.github.velox.desktop',
  install_dir: join_paths(get_option('datadir'), 'applications')
)

install_data(
  'io.github.velox.metainfo.xml',
  install_dir: join_paths(get_option('datadir'), 'metainfo')
)

gnome = import('gnome')
gnome.compile_schemas(build_by_default: true)

install_data(
  'io.github.velox.svg',
  install_dir: join_paths(get_option('datadir'), 'icons', 'hicolor', 'scalable', 'apps')
)
