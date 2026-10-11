# Copyright (c) 2022-2026, PostgreSQL Global Development Group

pg_lz4_sources = files(
  'pg_lz4.c',
)

if host_system == 'windows'
  pg_lz4_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'pg_lz4',
    '--FILEDESC', 'pg_lz4 - LZ4 compression and dictionary training for encoding contexts',])
endif

pg_lz4 = shared_module('pg_lz4',
  pg_lz4_sources,
  kwargs: contrib_mod_args,
)
contrib_targets += pg_lz4

install_data(
  'pg_lz4--1.0.sql',
  'pg_lz4.control',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'pg_lz4',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'pg_lz4',
    ],
  },
}
