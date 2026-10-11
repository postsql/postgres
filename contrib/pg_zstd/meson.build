# Copyright (c) 2022-2026, PostgreSQL Global Development Group

pg_zstd_sources = files(
  'pg_zstd.c',
)

if host_system == 'windows'
  pg_zstd_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'pg_zstd',
    '--FILEDESC', 'pg_zstd - Zstandard compression and dictionary training for encoding contexts',])
endif

pg_zstd = shared_module('pg_zstd',
  pg_zstd_sources,
  kwargs: contrib_mod_args,
)
contrib_targets += pg_zstd

install_data(
  'pg_zstd--1.0.sql',
  'pg_zstd.control',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'pg_zstd',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'pg_zstd',
    ],
  },
}
