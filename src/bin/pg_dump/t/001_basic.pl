
# Copyright (c) 2021-2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $tempdir = PostgreSQL::Test::Utils::tempdir;

#########################################
# Basic checks

program_help_ok('pg_dump');
program_version_ok('pg_dump');
program_options_handling_ok('pg_dump');

program_help_ok('pg_restore');
program_version_ok('pg_restore');
program_options_handling_ok('pg_restore');

program_help_ok('pg_dumpall');
program_version_ok('pg_dumpall');
program_options_handling_ok('pg_dumpall');

#########################################
# Test various invalid options and disallowed combinations
# Doesn't require a PG instance to be set up, so do this first.

command_fails_like(
	[ 'pg_dump', 'qqq', 'abc' ],
	qr/\Qpg_dump: error: too many command-line arguments (first is "abc")\E/,
	'pg_dump: too many command-line arguments');

command_fails_like(
	[ 'pg_restore', 'qqq', 'abc' ],
	qr/\Qpg_restore: error: too many command-line arguments (first is "abc")\E/,
	'pg_restore: too many command-line arguments');

command_fails_like(
	[ 'pg_dumpall', 'qqq', 'abc' ],
	qr/\Qpg_dumpall: error: too many command-line arguments (first is "qqq")\E/,
	'pg_dumpall: too many command-line arguments');

command_fails_like(
	[ 'pg_dump', '-s', '-a' ],
	qr/\Qpg_dump: error: options -a\/--data-only and -s\/--schema-only cannot be used together\E/,
	'pg_dump: options -a/--data-only and -s/--schema-only cannot be used together'
);

command_fails_like(
	[ 'pg_dump', '-s', '--statistics-only' ],
	qr/\Qpg_dump: error: options -s\/--schema-only and --statistics-only cannot be used together\E/,
	'pg_dump: error: options -s/--schema-only and --statistics-only cannot be used together'
);

command_fails_like(
	[ 'pg_dump', '-a', '--statistics-only' ],
	qr/\Qpg_dump: error: options -a\/--data-only and --statistics-only cannot be used together\E/,
	'pg_dump: error: options -a/--data-only and --statistics-only cannot be used together'
);

command_fails_like(
	[ 'pg_dump', '-s', '--include-foreign-data=xxx' ],
	qr/\Qpg_dump: error: options --include-foreign-data and -s\/--schema-only cannot be used together\E/,
	'pg_dump: options --include-foreign-data and -s/--schema-only cannot be used together'
);

command_fails_like(
	[ 'pg_dump', '--statistics-only', '--no-statistics' ],
	qr/\Qpg_dump: error: options --statistics-only and --no-statistics cannot be used together\E/,
	'pg_dump: options --statistics-only and --no-statistics cannot be used together'
);

command_fails_like(
	[ 'pg_dump', '-j2', '--include-foreign-data=xxx' ],
	qr/\Qpg_dump: error: option --include-foreign-data is not supported with parallel backup\E/,
	'pg_dump: option --include-foreign-data is not supported with parallel backup'
);

command_fails_like(
	['pg_restore'],
	qr{\Qpg_restore: error: one of -d/--dbname and -f/--file must be specified\E},
	'pg_restore: error: one of -d/--dbname and -f/--file must be specified');

command_fails_like(
	[ 'pg_restore', '-s', '-a', '-f -' ],
	qr/\Qpg_restore: error: options -s\/--schema-only and -a\/--data-only cannot be used together\E/,
	'pg_restore: options -s/--schema-only and -a/--data-only cannot be used together'
);

command_fails_like(
	[ 'pg_restore', '-d', 'xxx', '-f', 'xxx' ],
	qr/\Qpg_restore: error: options -d\/--dbname and -f\/--file cannot be used together\E/,
	'pg_restore: options -d/--dbname and -f/--file cannot be used together');

command_fails_like(
	[ 'pg_dump', '-c', '-a' ],
	qr/\Qpg_dump: error: options -c\/--clean and -a\/--data-only cannot be used together\E/,
	'pg_dump: options -c/--clean and -a/--data-only cannot be used together');

command_fails_like(
	[ 'pg_dumpall', '-c', '-a' ],
	qr/\Qpg_dumpall: error: options -c\/--clean and -a\/--data-only cannot be used together\E/,
	'pg_dumpall: options -c/--clean and -a/--data-only cannot be used together'
);

command_fails_like(
	[ 'pg_restore', '-c', '-a', '-f -' ],
	qr/\Qpg_restore: error: options -c\/--clean and -a\/--data-only cannot be used together\E/,
	'pg_restore: options -c/--clean and -a/--data-only cannot be used together'
);

command_fails_like(
	[ 'pg_dump', '--if-exists' ],
	qr/\Qpg_dump: error: option --if-exists requires option -c\/--clean\E/,
	'pg_dump: option --if-exists requires option -c/--clean');

command_fails_like(
	[ 'pg_dump', '-j3' ],
	qr/\Qpg_dump: error: parallel backup only supported by the directory format\E/,
	'pg_dump: parallel backup only supported by the directory format');

# Note the trailing whitespace for the value of --jobs, that is valid.
command_fails_like(
	[ 'pg_dump', '-j', '-1 ' ],
	qr/\Qpg_dump: error: -j\/--jobs must be in range\E/,
	'pg_dump: -j/--jobs must be in range');

command_fails_like(
	[ 'pg_dump', '-F', 'garbage' ],
	qr/\Qpg_dump: error: invalid output format\E/,
	'pg_dump: invalid output format');

command_fails_like(
	[ 'pg_restore', '-j', '-1', '-f -' ],
	qr/\Qpg_restore: error: -j\/--jobs must be in range\E/,
	'pg_restore: -j/--jobs must be in range');

command_fails_like(
	[ 'pg_restore', '--single-transaction', '-j3', '-f -' ],
	qr/\Qpg_restore: error: cannot specify both --single-transaction and multiple jobs\E/,
	'pg_restore: cannot specify both --single-transaction and multiple jobs');

command_fails_like(
	[ 'pg_dump', '--compress', 'garbage' ],
	qr/\Qpg_dump: error: unrecognized compression algorithm/,
	'pg_dump: invalid --compress');

command_fails_like(
	[ 'pg_dump', '--compress', 'none:1' ],
	qr/\Qpg_dump: error: invalid compression specification: compression algorithm "none" does not accept a compression level\E/,
	'pg_dump: invalid compression specification: compression algorithm "none" does not accept a compression level'
);


if (check_pg_config("#define HAVE_LIBZ 1"))
{
	command_fails_like(
		[ 'pg_dump', '-Z', '15' ],
		qr/\Qpg_dump: error: invalid compression specification: compression algorithm "gzip" expects a compression level between 1 and 9 (default at -1)\E/,
		'pg_dump: invalid compression specification: must be in range');

	command_fails_like(
		[ 'pg_dump', '--compress', '1', '--format', 'tar' ],
		qr/\Qpg_dump: error: compression is not supported by tar archive format\E/,
		'pg_dump: compression is not supported by tar archive format');

	command_fails_like(
		[ 'pg_dump', '-Z', 'gzip:nonInt' ],
		qr/\Qpg_dump: error: invalid compression specification: unrecognized compression option: "nonInt"\E/,
		'pg_dump: invalid compression specification: must be an integer');
}
else
{
	# --jobs > 1 forces an error with tar format.
	command_fails_like(
		[ 'pg_dump', '--format', 'tar', '-j3' ],
		qr/\Qpg_dump: error: parallel backup only supported by the directory format\E/,
		'pg_dump: warning: parallel backup not supported by tar format');

	command_fails_like(
		[ 'pg_dump', '-Z', 'gzip:nonInt', '--format', 'tar', '-j2' ],
		qr/\Qpg_dump: error: invalid compression specification: unrecognized compression option\E/,
		'pg_dump: invalid compression specification: must be an integer');
}

command_fails_like(
	[ 'pg_dump', '--extra-float-digits', '-16' ],
	qr/\Qpg_dump: error: --extra-float-digits must be in range\E/,
	'pg_dump: --extra-float-digits must be in range');

command_fails_like(
	[ 'pg_dump', '--rows-per-insert', '0' ],
	qr/\Qpg_dump: error: --rows-per-insert must be in range\E/,
	'pg_dump: --rows-per-insert must be in range');

command_fails_like(
	[ 'pg_restore', '--if-exists', '-f -' ],
	qr/\Qpg_restore: error: option --if-exists requires option -c\/--clean\E/,
	'pg_restore: option --if-exists requires option -c/--clean');

command_fails_like(
	[ 'pg_restore', '-f -', '-F', 'garbage' ],
	qr/\Qpg_restore: error: unrecognized archive format "garbage";\E/,
	'pg_dump: unrecognized archive format');

command_fails_like(
	[ 'pg_dump', '--on-conflict-do-nothing' ],
	qr/pg_dump: error: option --on-conflict-do-nothing requires option --inserts, --rows-per-insert, or --column-inserts/,
	'pg_dump: --on-conflict-do-nothing requires --inserts, --rows-per-insert, --column-inserts'
);

# pg_dumpall command-line argument checks
command_fails_like(
	[ 'pg_dumpall', '-g', '-r' ],
	qr/\Qpg_dumpall: error: options -g\/--globals-only and -r\/--roles-only cannot be used together\E/,
	'pg_dumpall: options -g/--globals-only and -r/--roles-only cannot be used together'
);

command_fails_like(
	[ 'pg_dumpall', '-g', '-t' ],
	qr/\Qpg_dumpall: error: options -g\/--globals-only and -t\/--tablespaces-only cannot be used together\E/,
	'pg_dumpall: options -g/--globals-only and -t/--tablespaces-only cannot be used together'
);

command_fails_like(
	[ 'pg_dumpall', '-r', '-t' ],
	qr/\Qpg_dumpall: error: options -r\/--roles-only and -t\/--tablespaces-only cannot be used together\E/,
	'pg_dumpall: options -r/--roles-only and -t/--tablespaces-only cannot be used together'
);

command_fails_like(
	[ 'pg_dumpall', '--if-exists' ],
	qr/\Qpg_dumpall: error: option --if-exists requires option -c\/--clean\E/,
	'pg_dumpall: option --if-exists requires option -c/--clean');

command_fails_like(
	[ 'pg_restore', '-C', '-1', '-f -' ],
	qr/\Qpg_restore: error: options -C\/--create and -1\/--single-transaction cannot be used together\E/,
	'pg_restore: options -C\/--create and -1\/--single-transaction cannot be used together'
);

# also fails for -r and -t, but it seems pointless to add more tests for those.
command_fails_like(
	[ 'pg_dumpall', '--exclude-database=foo', '--globals-only' ],
	qr/\Qpg_dumpall: error: option --exclude-database cannot be used together with -g\/--globals-only\E/,
	'pg_dumpall: option --exclude-database cannot be used together with -g/--globals-only'
);

# Table chunking CLI options and PGDUMP_* environment variable validation
command_fails_like(
	[ 'pg_dump', '--max-table-segment-pages=0' ],
	qr/\Qpg_dump: error: --max-table-segment-pages not in range 1..4294967294\E/,
	'pg_dump: --max-table-segment-pages=0 out of range');

command_fails_like(
	[ 'pg_dump', '--max-table-segment-pages=-5' ],
	qr/\Qpg_dump: error: value "-5" for option --max-table-segment-pages can not be negative\E/,
	'pg_dump: --max-table-segment-pages=-5 out of range');

command_fails_like(
	[ 'pg_dump', '--max-table-segment-pages=4294967295' ],
	qr/\Qpg_dump: error: --max-table-segment-pages not in range 1..4294967294\E/,
	'pg_dump: --max-table-segment-pages=4294967295 out of range');

command_fails_like(
	[ 'pg_dump', '--max-table-segment-pages=invalid' ],
	qr/\Qpg_dump: error: invalid value "invalid" for option --max-table-segment-pages\E/,
	'pg_dump: --max-table-segment-pages=invalid');

command_fails_like(
	[ 'pg_dump', '--max_table_segment_pages=invalid' ],
	qr/\Qpg_dump: error: invalid value "invalid" for option --max-table-segment-pages\E/,
	'pg_dump: --max_table_segment_pages=invalid');

command_fails_like(
	[ 'pg_dump', '--chunk-above-size=0' ],
	qr/\Qpg_dump: error: invalid value for --chunk-above-size: "0"\E/,
	'pg_dump: --chunk-above-size=0 invalid');

command_fails_like(
	[ 'pg_dump', '--chunk_above_size=10XB' ],
	qr/\Qpg_dump: error: invalid value for --chunk-above-size: "10XB"\E/,
	'pg_dump: --chunk_above_size=10XB invalid');

command_fails_like(
	[ 'pg_dump', '--chunk-size=0MB' ],
	qr/\Qpg_dump: error: invalid value for --chunk-size: "0MB"\E/,
	'pg_dump: --chunk-size=0MB invalid');

command_fails_like(
	[ 'pg_dump', '--chunk_size=-5M' ],
	qr/\Qpg_dump: error: invalid value for --chunk-size: "-5M"\E/,
	'pg_dump: --chunk_size=-5M invalid');

command_fails_like(
	[ 'pg_dump', '--table-chunk-sizes=missing_colon' ],
	qr/\Qpg_dump: error: invalid value for --table-chunk-sizes: "missing_colon"\E/,
	'pg_dump: --table-chunk-sizes=missing_colon invalid');

command_fails_like(
	[ 'pg_dump', '--table_chunk_sizes=a.b.c:10M' ],
	qr/\Qpg_dump: error: invalid value for --table-chunk-sizes: "a.b.c:10M"\E/,
	'pg_dump: --table_chunk_sizes=a.b.c:10M invalid');

command_fails_like(
	[ 'pg_dump', '--table-chunk-sizes=' ],
	qr/\Qpg_dump: error: invalid value for --table-chunk-sizes: ""\E/,
	'pg_dump: --table-chunk-sizes= empty invalid');

command_fails_like(
	[ 'pg_dump', '--table-chunk-sizes=   ' ],
	qr/\Qpg_dump: error: invalid value for --table-chunk-sizes: "   "\E/,
	'pg_dump: --table-chunk-sizes= whitespace invalid');

command_fails_like(
	[ 'pg_dump', '--settings-from-database=maybe' ],
	qr/\Qpg_dump: error: invalid value "maybe" for option --settings-from-database\E/,
	'pg_dump: --settings-from-database=maybe invalid');

command_fails_like(
	[ 'pg_dump', '--settings_from_database=2' ],
	qr/\Qpg_dump: error: invalid value "2" for option --settings-from-database\E/,
	'pg_dump: --settings_from_database=2 invalid');

command_fails_like(
	[ 'pg_dump', '--binary-upgrade', '--max-table-segment-pages=10' ],
	qr/\Qpg_dump: error: table chunking options and --binary-upgrade cannot be used together\E/,
	'pg_dump: --binary-upgrade with --max-table-segment-pages fails');

command_fails_like(
	[ 'pg_dump', '--binary-upgrade', '--chunk-size=16MB' ],
	qr/\Qpg_dump: error: table chunking options and --binary-upgrade cannot be used together\E/,
	'pg_dump: --binary-upgrade with --chunk-size fails');

command_fails_like(
	[ 'pg_dumpall', '--max-table-segment-pages=0' ],
	qr/\Qpg_dumpall: error: --max-table-segment-pages not in range 1..4294967294\E/,
	'pg_dumpall: --max-table-segment-pages=0 out of range');

command_fails_like(
	[ 'pg_dumpall', '--max-table-segment-pages=invalid' ],
	qr/\Qpg_dumpall: error: invalid value "invalid" for option --max-table-segment-pages\E/,
	'pg_dumpall: --max-table-segment-pages=invalid');

command_fails_like(
	[ 'pg_dumpall', '--chunk-above-size=invalid' ],
	qr/\Qpg_dumpall: error: invalid value for --chunk-above-size: "invalid"\E/,
	'pg_dumpall: --chunk-above-size=invalid');

command_fails_like(
	[ 'pg_dumpall', '--chunk-size=invalid' ],
	qr/\Qpg_dumpall: error: invalid value for --chunk-size: "invalid"\E/,
	'pg_dumpall: --chunk-size=invalid');

command_fails_like(
	[ 'pg_dumpall', '--table-chunk-sizes=invalid' ],
	qr/\Qpg_dumpall: error: invalid value for --table-chunk-sizes: "invalid"\E/,
	'pg_dumpall: --table-chunk-sizes=invalid');

command_fails_like(
	[ 'pg_dumpall', '--table-chunk-sizes=' ],
	qr/\Qpg_dumpall: error: invalid value for --table-chunk-sizes: ""\E/,
	'pg_dumpall: --table-chunk-sizes= empty invalid');

command_fails_like(
	[ 'pg_dumpall', '--table-chunk-sizes=   ' ],
	qr/\Qpg_dumpall: error: invalid value for --table-chunk-sizes: "   "\E/,
	'pg_dumpall: --table-chunk-sizes= whitespace invalid');

command_fails_like(
	[ 'pg_dumpall', '--settings-from-database=invalid' ],
	qr/\Qpg_dumpall: error: invalid value "invalid" for option --settings-from-database\E/,
	'pg_dumpall: --settings-from-database=invalid');

{
	local $ENV{PGDUMP_MAX_TABLE_SEGMENT_PAGES} = 'bad_env_val';
	command_fails_like(
		[ 'pg_dump' ],
		qr/\Qpg_dump: error: invalid value "bad_env_val" for option PGDUMP_MAX_TABLE_SEGMENT_PAGES\E/,
		'pg_dump: invalid PGDUMP_MAX_TABLE_SEGMENT_PAGES environment variable');

	command_fails_like(
		[ 'pg_dumpall' ],
		qr/\Qpg_dumpall: error: invalid value "bad_env_val" for option PGDUMP_MAX_TABLE_SEGMENT_PAGES\E/,
		'pg_dumpall: invalid PGDUMP_MAX_TABLE_SEGMENT_PAGES environment variable');

	command_fails_like(
		[ 'pg_dump', '--max-table-segment-pages=10', '-s', '-a' ],
		qr/\Qpg_dump: error: options -a\/--data-only and -s\/--schema-only cannot be used together\E/,
		'pg_dump: CLI --max-table-segment-pages=10 overrides PGDUMP_MAX_TABLE_SEGMENT_PAGES');
}

{
	local $ENV{PGDUMP_CHUNK_ABOVE_SIZE} = 'bad_above';
	command_fails_like(
		[ 'pg_dump' ],
		qr/\Qpg_dump: error: invalid value for PGDUMP_CHUNK_ABOVE_SIZE: "bad_above"\E/,
		'pg_dump: invalid PGDUMP_CHUNK_ABOVE_SIZE environment variable');

	command_fails_like(
		[ 'pg_dumpall' ],
		qr/\Qpg_dumpall: error: invalid value for PGDUMP_CHUNK_ABOVE_SIZE: "bad_above"\E/,
		'pg_dumpall: invalid PGDUMP_CHUNK_ABOVE_SIZE environment variable');
}

{
	local $ENV{PGDUMP_CHUNK_SIZE} = 'bad_size';
	command_fails_like(
		[ 'pg_dump' ],
		qr/\Qpg_dump: error: invalid value for PGDUMP_CHUNK_SIZE: "bad_size"\E/,
		'pg_dump: invalid PGDUMP_CHUNK_SIZE environment variable');

	command_fails_like(
		[ 'pg_dumpall' ],
		qr/\Qpg_dumpall: error: invalid value for PGDUMP_CHUNK_SIZE: "bad_size"\E/,
		'pg_dumpall: invalid PGDUMP_CHUNK_SIZE environment variable');
}

{
	local $ENV{PGDUMP_TABLE_CHUNK_SIZES} = 'bad_table_spec';
	command_fails_like(
		[ 'pg_dump' ],
		qr/\Qpg_dump: error: invalid value for PGDUMP_TABLE_CHUNK_SIZES: "bad_table_spec"\E/,
		'pg_dump: invalid PGDUMP_TABLE_CHUNK_SIZES environment variable');

	command_fails_like(
		[ 'pg_dumpall' ],
		qr/\Qpg_dumpall: error: invalid value for PGDUMP_TABLE_CHUNK_SIZES: "bad_table_spec"\E/,
		'pg_dumpall: invalid PGDUMP_TABLE_CHUNK_SIZES environment variable');
}

{
	local $ENV{PGDUMP_TABLE_CHUNK_SIZES} = '   ';
	command_fails_like(
		[ 'pg_dump' ],
		qr/\Qpg_dump: error: invalid value for PGDUMP_TABLE_CHUNK_SIZES: "   "\E/,
		'pg_dump: whitespace-only PGDUMP_TABLE_CHUNK_SIZES environment variable');

	command_fails_like(
		[ 'pg_dumpall' ],
		qr/\Qpg_dumpall: error: invalid value for PGDUMP_TABLE_CHUNK_SIZES: "   "\E/,
		'pg_dumpall: whitespace-only PGDUMP_TABLE_CHUNK_SIZES environment variable');
}

{
	local $ENV{PGDUMP_SETTINGS_FROM_DATABASE} = 'bad_bool';
	command_fails_like(
		[ 'pg_dump' ],
		qr/\Qpg_dump: error: invalid value "bad_bool" for option PGDUMP_SETTINGS_FROM_DATABASE\E/,
		'pg_dump: invalid PGDUMP_SETTINGS_FROM_DATABASE environment variable');

	command_fails_like(
		[ 'pg_dumpall' ],
		qr/\Qpg_dumpall: error: invalid value "bad_bool" for option PGDUMP_SETTINGS_FROM_DATABASE\E/,
		'pg_dumpall: invalid PGDUMP_SETTINGS_FROM_DATABASE environment variable');

	command_fails_like(
		[ 'pg_dump', '--settings-from-database=off', '-s', '-a' ],
		qr/\Qpg_dump: error: options -a\/--data-only and -s\/--schema-only cannot be used together\E/,
		'pg_dump: CLI --settings-from-database=off overrides PGDUMP_SETTINGS_FROM_DATABASE');
}

done_testing();
