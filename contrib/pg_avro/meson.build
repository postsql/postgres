# Copyright (c) 2022-2026, PostgreSQL Global Development Group

pg_avro_sources = files(
  'pg_avro.c',
)

if host_system == 'windows'
  pg_avro_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'pg_avro',
    '--FILEDESC', 'pg_avro - Apache Avro binary serialization for encoding contexts',])
endif

pg_avro = shared_module('pg_avro',
  pg_avro_sources,
  kwargs: contrib_mod_args,
)
contrib_targets += pg_avro

install_data(
  'pg_avro--1.0.sql',
  'pg_avro.control',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'pg_avro',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'pg_avro',
    ],
  },
}
