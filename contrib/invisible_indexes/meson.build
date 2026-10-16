# Copyright (c) 2026, Google LLC.

invisible_indexes_sources = files(
  'invisible_indexes.c',
)

if host_system == 'windows'
  invisible_indexes_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'invisible_indexes',
    '--FILEDESC', 'invisible_indexes - support for invisible and excluded indexes',])
endif

invisible_indexes = shared_module('invisible_indexes',
  invisible_indexes_sources,
  kwargs: contrib_mod_args,
)
contrib_targets += invisible_indexes

install_data(
  'invisible_indexes--1.0.sql',
  'invisible_indexes.control',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'invisible_indexes',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'invisible_indexes',
    ],
    'regress_args': [
      '--temp-config', files('invisible_indexes_regression_test_settings.conf'),
    ],
  },
}
