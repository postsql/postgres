# Copyright (c) 2026, PostgreSQL Global Development Group

pg_cascade_sources = files(
  'pg_cascade.c',
)

if host_system == 'windows'
  pg_cascade_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'pg_cascade',
    '--FILEDESC', 'pg_cascade - multi-stage cascade and fused Avro+Zstd encoding context handlers',])
endif

pg_cascade = shared_module('pg_cascade',
  pg_cascade_sources,
  kwargs: contrib_mod_args + {
    'dependencies': contrib_mod_args['dependencies'] + [zstd, lz4],
  },
)
contrib_targets += pg_cascade

install_data(
  'pg_cascade.control',
  'pg_cascade--1.0.sql',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'pg_cascade',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'pg_cascade',
    ],
  },
}
