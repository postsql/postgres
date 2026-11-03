
# Copyright (c) 2021-2026, PostgreSQL Global Development Group

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $dbname1 = 'regression_src';
my $dbname2 = 'regression_dest1';
my $dbname3 = 'regression_dest2';
my $dbname4 = 'regression_dest3';
my $dbname5 = 'regression_dest4';

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->start;

my $backupdir = $node->backup_dir;

$node->run_log([ 'createdb', $dbname1 ]);
$node->run_log([ 'createdb', $dbname2 ]);
$node->run_log([ 'createdb', $dbname3 ]);
$node->run_log([ 'createdb', $dbname4 ]);
$node->run_log([ 'createdb', $dbname5 ]);

$node->safe_psql(
	$dbname1,
	qq{
create type digit as enum ('0', '1', '2', '3', '4', '5', '6', '7', '8', '9');

-- plain table with index
create table tplain (en digit, data int unique);
insert into tplain select (x%10)::text::digit, x from generate_series(1,1000) x;

-- non-troublesome hashed partitioning
create table ths (mod int, data int, unique(mod, data)) partition by hash(mod);
create table ths_p1 partition of ths for values with (modulus 3, remainder 0);
create table ths_p2 partition of ths for values with (modulus 3, remainder 1);
create table ths_p3 partition of ths for values with (modulus 3, remainder 2);
insert into ths select (x%10), x from generate_series(1,1000) x;

-- dangerous hashed partitioning
create table tht (en digit, data int, unique(en, data)) partition by hash(en);
create table tht_p1 partition of tht for values with (modulus 3, remainder 0);
create table tht_p2 partition of tht for values with (modulus 3, remainder 1);
create table tht_p3 partition of tht for values with (modulus 3, remainder 2);
insert into tht select (x%10)::text::digit, x from generate_series(1,1000) x;
	});

$node->command_ok(
	[
		'pg_dump',
		'--format' => 'directory',
		'--no-sync',
		'--jobs' => 2,
		'--file' => "$backupdir/dump1",
		$node->connstr($dbname1),
	],
	'parallel dump');

$node->command_ok(
	[
		'pg_restore', '--verbose',
		'--dbname' => $node->connstr($dbname2),
		'--jobs' => 3,
		"$backupdir/dump1",
	],
	'parallel restore');

$node->command_ok(
	[
		'pg_dump',
		'--format' => 'directory',
		'--no-sync',
		'--jobs' => 2,
		'--file' => "$backupdir/dump2",
		'--inserts',
		$node->connstr($dbname1),
	],
	'parallel dump as inserts');

$node->command_ok(
	[
		'pg_restore', '--verbose',
		'--dbname' => $node->connstr($dbname3),
		'--jobs' => 3,
		"$backupdir/dump2",
	],
	'parallel restore as inserts');

$node->command_ok(
	[
		'pg_dump',
		'--format' => 'directory',
		'--max-table-segment-pages' => 2,
		'--no-sync',
		'--jobs' => 2,
		'--file' => "$backupdir/dump3",
		$node->connstr($dbname1),
	],
	'parallel dump with chunks of two heap pages');

$node->command_ok(
	[
		'pg_restore', '--verbose',
		'--dbname' => $node->connstr($dbname4),
		'--jobs' => 3,
		"$backupdir/dump3",
	],
	'parallel restore with chunks of two heap pages');

my $table = 'tplain';
my $tablehash_query = "SELECT '$table', sum(hashtext(t::text)), count(*) FROM $table AS t";

my $result_1 = $node->safe_psql($dbname1, $tablehash_query);
my $result_4 = $node->safe_psql($dbname4, $tablehash_query);

is($result_4, $result_1, "Hash check for $table: restored db ($result_4) vs original db ($result_1)");

# CLI validation for --max-table-segment-pages
command_fails_like(
	[ 'pg_dump', '--max-table-segment-pages=0' ],
	qr/\Qpg_dump: error: --max-table-segment-pages not in range 1..4294967294\E/,
	'pg_dump: --max-table-segment-pages=0 out of range');

command_fails_like(
	[ 'pg_dump', '--max-table-segment-pages=-5' ],
	qr/\Qpg_dump: error: value "-5" for option --max-table-segment-pages can not be negative\E/,
	'pg_dump: --max-table-segment-pages=-5 negative rejected');

command_fails_like(
	[ 'pg_dump', '--max-table-segment-pages=4294967295' ],
	qr/\Qpg_dump: error: --max-table-segment-pages not in range 1..4294967294\E/,
	'pg_dump: --max-table-segment-pages=4294967295 out of range');

command_fails_like(
	[ 'pg_dump', '--max-table-segment-pages=invalid' ],
	qr/\Qpg_dump: error: invalid value "invalid" for option --max-table-segment-pages\E/,
	'pg_dump: --max-table-segment-pages=invalid');

# Boundary heap-table chunking cases: 0-page empty table, 1-page table, and
# exact multiple of --max-table-segment-pages (4 pages with 2-page segments).
{
	my $dbname_bounds_src = 'regression_chunk_bounds_src';
	my $dbname_bounds_dest = 'regression_chunk_bounds_dest';
	$node->run_log([ 'createdb', $dbname_bounds_src ]);
	$node->run_log([ 'createdb', $dbname_bounds_dest ]);

	$node->safe_psql(
		$dbname_bounds_src,
		qq{
CREATE TABLE public.tempty (id int PRIMARY KEY, payload text);

CREATE TABLE public.tonepage (id int PRIMARY KEY, payload text);
INSERT INTO public.tonepage SELECT g, 'row_' || g FROM generate_series(1, 5) g;

CREATE TABLE public.texact4 (id int PRIMARY KEY, payload text) WITH (fillfactor = 10);
INSERT INTO public.texact4 SELECT g, repeat('x', 450) FROM generate_series(1, 4) g;

VACUUM ANALYZE;
		});

	my $exact4_pages = $node->safe_psql(
		$dbname_bounds_src,
		"SELECT pg_relation_size('public.texact4') / current_setting('block_size')::int"
	);
	is($exact4_pages, '4', 'texact4 occupies exactly 4 heap pages');

	$node->command_checks_all(
		[
			'pg_dump', '-v', '-t', 'public.tonepage', '--data-only',
			'--max-table-segment-pages=1',
			$node->connstr($dbname_bounds_src),
		],
		0,
		[
			qr/Name: tonepage; Type: TABLE DATA; Schema: public/,
			qr/^(?!.*Type: TABLE DATA \(pages)/s,
		],
		[ qr// ],
		'1-page table is not chunked even when --max-table-segment-pages=1');

	$node->command_checks_all(
		[
			'pg_dump', '-v', '--data-only',
			'--max-table-segment-pages=2',
			$node->connstr($dbname_bounds_src),
		],
		0,
		[
			qr/Name: tempty; Type: TABLE DATA; Schema: public/,
			qr/Name: tonepage; Type: TABLE DATA; Schema: public/,
			qr/Name: texact4; Type: TABLE DATA \(pages 0:1\); Schema: public/,
			qr/Name: texact4; Type: TABLE DATA \(pages 2:4294967295\); Schema: public/,
			qr/^(?!.*pages 4:)/s,
		],
		[ qr// ],
		'0-page, 1-page, and exact 4-page tables emit expected TOC entries with no trailing empty chunk');

	$node->command_ok(
		[
			'pg_dump',
			'--format' => 'directory',
			'--max-table-segment-pages' => 2,
			'--no-sync',
			'--jobs' => 2,
			'--file' => "$backupdir/dump_bounds",
			$node->connstr($dbname_bounds_src),
		],
		'parallel dump of boundary 0-page, 1-page, and exact 4-page tables');

	$node->command_ok(
		[
			'pg_restore', '--verbose',
			'--dbname' => $node->connstr($dbname_bounds_dest),
			'--jobs' => 3,
			"$backupdir/dump_bounds",
		],
		'parallel restore of boundary 0-page, 1-page, and exact 4-page tables');

	my $bounds_summary_query = q{
SELECT 'tempty', count(*), coalesce(sum(hashtext(t::text)), 0) FROM public.tempty t
UNION ALL
SELECT 'tonepage', count(*), coalesce(sum(hashtext(t::text)), 0) FROM public.tonepage t
UNION ALL
SELECT 'texact4', count(*), coalesce(sum(hashtext(t::text)), 0) FROM public.texact4 t
ORDER BY 1;
	};
	my $bounds_src_res = $node->safe_psql($dbname_bounds_src, $bounds_summary_query);
	my $bounds_dest_res = $node->safe_psql($dbname_bounds_dest, $bounds_summary_query);
	is($bounds_dest_res, $bounds_src_res,
		'restored boundary tables match original counts and hashes');
}

$node->command_ok(
	[
		'pg_dump',
		'--format' => 'directory',
		'--max-table-segment-pages' => 2,
		'--inserts',
		'--no-sync',
		'--jobs' => 2,
		'--file' => "$backupdir/dump4",
		$node->connstr($dbname1),
	],
	'parallel dump with chunks of two heap pages using inserts');

$node->command_ok(
	[
		'pg_restore', '--verbose',
		'--dbname' => $node->connstr($dbname5),
		'--jobs' => 3,
		"$backupdir/dump4",
	],
	'parallel restore with chunks of two heap pages using inserts');

my $result_5 = $node->safe_psql($dbname5, $tablehash_query);
is($result_5, $result_1, "Hash check for $table (inserts): restored db ($result_5) vs original db ($result_1)");

#########################################
# Unified table chunking configuration tests:
# 1. Database (pgdump.*) > 2. Command Line (--*) > 3. Environment (PGDUMP_*)
# and --settings-from-database / PGDUMP_SETTINGS_FROM_DATABASE control flag

# 1. CLI and environment variable validation errors
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
	[ 'pg_dumpall', '--settings-from-database=invalid' ],
	qr/\Qpg_dumpall: error: invalid value "invalid" for option --settings-from-database\E/,
	'pg_dumpall: --settings-from-database=invalid');

command_fails_like(
	[ 'pg_dumpall', '--binary-upgrade', '--chunk-size=16MB' ],
	qr/\Qpg_dumpall: error: table chunking options and --binary-upgrade cannot be used together\E/,
	'pg_dumpall: --binary-upgrade with --chunk-size fails');

{
	local $ENV{PGDUMP_MAX_TABLE_SEGMENT_PAGES} = 'invalid_env';
	command_fails_like(
		[ 'pg_dump', $node->connstr($dbname1) ],
		qr/\Qpg_dump: error: invalid value "invalid_env" for option PGDUMP_MAX_TABLE_SEGMENT_PAGES\E/,
		'pg_dump: invalid PGDUMP_MAX_TABLE_SEGMENT_PAGES fails');

	command_fails_like(
		[ 'pg_dumpall', '--database' => $node->connstr($dbname1) ],
		qr/\Qpg_dumpall: error: invalid value "invalid_env" for option PGDUMP_MAX_TABLE_SEGMENT_PAGES\E/,
		'pg_dumpall: invalid PGDUMP_MAX_TABLE_SEGMENT_PAGES fails');
}

{
	local $ENV{PGDUMP_CHUNK_ABOVE_SIZE} = 'bad_above';
	command_fails_like(
		[ 'pg_dump', $node->connstr($dbname1) ],
		qr/\Qpg_dump: error: invalid value for PGDUMP_CHUNK_ABOVE_SIZE: "bad_above"\E/,
		'pg_dump: invalid PGDUMP_CHUNK_ABOVE_SIZE fails');

	command_fails_like(
		[ 'pg_dumpall', '--database' => $node->connstr($dbname1) ],
		qr/\Qpg_dumpall: error: invalid value for PGDUMP_CHUNK_ABOVE_SIZE: "bad_above"\E/,
		'pg_dumpall: invalid PGDUMP_CHUNK_ABOVE_SIZE fails');
}

{
	local $ENV{PGDUMP_CHUNK_SIZE} = 'bad_size';
	command_fails_like(
		[ 'pg_dump', $node->connstr($dbname1) ],
		qr/\Qpg_dump: error: invalid value for PGDUMP_CHUNK_SIZE: "bad_size"\E/,
		'pg_dump: invalid PGDUMP_CHUNK_SIZE fails');

	command_fails_like(
		[ 'pg_dumpall', '--database' => $node->connstr($dbname1) ],
		qr/\Qpg_dumpall: error: invalid value for PGDUMP_CHUNK_SIZE: "bad_size"\E/,
		'pg_dumpall: invalid PGDUMP_CHUNK_SIZE fails');
}

{
	local $ENV{PGDUMP_TABLE_CHUNK_SIZES} = 'bad_table_spec';
	command_fails_like(
		[ 'pg_dump', $node->connstr($dbname1) ],
		qr/\Qpg_dump: error: invalid value for PGDUMP_TABLE_CHUNK_SIZES: "bad_table_spec"\E/,
		'pg_dump: invalid PGDUMP_TABLE_CHUNK_SIZES fails');

	command_fails_like(
		[ 'pg_dumpall', '--database' => $node->connstr($dbname1) ],
		qr/\Qpg_dumpall: error: invalid value for PGDUMP_TABLE_CHUNK_SIZES: "bad_table_spec"\E/,
		'pg_dumpall: invalid PGDUMP_TABLE_CHUNK_SIZES fails');
}

{
	local $ENV{PGDUMP_SETTINGS_FROM_DATABASE} = 'bad_bool';
	command_fails_like(
		[ 'pg_dump', $node->connstr($dbname1) ],
		qr/\Qpg_dump: error: invalid value "bad_bool" for option PGDUMP_SETTINGS_FROM_DATABASE\E/,
		'pg_dump: invalid PGDUMP_SETTINGS_FROM_DATABASE fails');

	command_fails_like(
		[ 'pg_dumpall', '--database' => $node->connstr($dbname1) ],
		qr/\Qpg_dumpall: error: invalid value "bad_bool" for option PGDUMP_SETTINGS_FROM_DATABASE\E/,
		'pg_dumpall: invalid PGDUMP_SETTINGS_FROM_DATABASE fails');
}

# 2. PGDUMP_MAX_TABLE_SEGMENT_PAGES environment variable and CLI override precedence
{
	local $ENV{PGDUMP_MAX_TABLE_SEGMENT_PAGES} = '2';
	$node->command_checks_all(
		[
			'pg_dump', '-v', '-t', 'tplain', '--data-only',
			$node->connstr($dbname1),
		],
		0,
		[ qr/Type: TABLE DATA \(pages 0:1\)/, qr/Type: TABLE DATA \(pages 2:3\)/ ],
		[ qr// ],
		'PGDUMP_MAX_TABLE_SEGMENT_PAGES=2 chunks tplain into 2-page segments');

	# CLI flag overrides PGDUMP_MAX_TABLE_SEGMENT_PAGES
	local $ENV{PGDUMP_MAX_TABLE_SEGMENT_PAGES} = '1';
	$node->command_checks_all(
		[
			'pg_dump', '-v', '-t', 'tplain', '--data-only',
			'--max-table-segment-pages=1000',
			$node->connstr($dbname1),
		],
		0,
		[ qr/Type: TABLE DATA; Schema: public/ ],
		[ qr// ],
		'CLI --max-table-segment-pages=1000 overrides PGDUMP_MAX_TABLE_SEGMENT_PAGES=1');
}

# 3. Create a dedicated database to test in-database pgdump.*, CLI, and Env chunking settings
my $dbname_chunk = 'regression_db_chunk';
my $dbname_chunk_restore = 'regression_db_chunk_restore';
$node->run_log([ 'createdb', $dbname_chunk ]);
$node->run_log([ 'createdb', $dbname_chunk_restore ]);

$node->safe_psql(
	$dbname_chunk,
	qq{
CREATE SCHEMA s1;
CREATE SCHEMA s2;

-- Small 1-page table (~8kB)
CREATE TABLE public.tsmall (id int, val int);
INSERT INTO public.tsmall VALUES (1, 10);

-- Medium 6-page heap table without TOAST (~48kB main heap, reltoastrelid = 0)
CREATE TABLE public.tmed (id int, c1 bigint, c2 bigint, c3 bigint, c4 bigint) WITH (fillfactor = 10);
INSERT INTO public.tmed SELECT g, g, g * 2, g * 3, g * 4 FROM generate_series(1, 60) g;

-- Table with large uncompressed external TOAST:
-- Main heap is ~4 pages (32kB < 64kB), TOAST is ~240kB, total pg_table_size ~300kB
CREATE TABLE public.ttoast (id int, payload text) WITH (fillfactor = 10);
ALTER TABLE public.ttoast ALTER COLUMN payload SET STORAGE EXTERNAL;
INSERT INTO public.ttoast SELECT g, repeat('z', 3500) FROM generate_series(1, 60) g;

-- Schema-qualified tables with the same relname (no TOAST)
CREATE TABLE s1.shared_tbl (id int, c1 bigint, c2 bigint, c3 bigint, c4 bigint) WITH (fillfactor = 10);
INSERT INTO s1.shared_tbl SELECT g, g, g * 2, g * 3, g * 4 FROM generate_series(1, 60) g;

CREATE TABLE s2.shared_tbl (id int, c1 bigint, c2 bigint, c3 bigint, c4 bigint) WITH (fillfactor = 10);
INSERT INTO s2.shared_tbl SELECT g, g, g * 2, g * 3, g * 4 FROM generate_series(1, 60) g;

VACUUM ANALYZE;
	});

# 3a. Default settings-from-database=on with no pgdump.* settings configured -> no chunking
$node->command_checks_all(
	[
		'pg_dump', '-v', '-t', 'public.tmed', '--data-only',
		$node->connstr($dbname_chunk),
	],
	0,
	[ qr/Name: tmed; Type: TABLE DATA; Schema: public/ ],
	[ qr// ],
	'default settings-from-database=on with no DB settings does not chunk');

# 3b. pgdump.max_table_segment_pages (and hyphenated pgdump.max-table-segment-pages via PGOPTIONS)
# read automatically by default (--settings-from-database=on)
$node->safe_psql($dbname_chunk, "ALTER DATABASE $dbname_chunk SET pgdump.max_table_segment_pages = '2'");
$node->command_checks_all(
	[
		'pg_dump', '-v', '-t', 'public.tmed', '--data-only',
		$node->connstr($dbname_chunk),
	],
	0,
	[ qr/Name: tmed; Type: TABLE DATA \(pages 0:1\); Schema: public/ ],
	[ qr// ],
	'default settings-from-database=on uses pgdump.max_table_segment_pages');

{
	local $ENV{PGOPTIONS} = '-c pgdump.max-table-segment-pages=3';
	$node->command_checks_all(
		[
			'pg_dump', '-v', '-t', 'public.tmed', '--data-only',
			$node->connstr($dbname_chunk),
		],
		0,
		[ qr/Name: tmed; Type: TABLE DATA \(pages 0:2\); Schema: public/ ],
		[ qr// ],
		'settings-from-database=on supports hyphenated pgdump.max-table-segment-pages');
}

# Verify 1. Database > 2. Command Line > 3. Environment for max-table-segment-pages,
# and verify --settings-from-database=off / PGDUMP_SETTINGS_FROM_DATABASE=off
{
	local $ENV{PGDUMP_MAX_TABLE_SEGMENT_PAGES} = '1';

	# With settings-from-database=on (default), DB setting (2) beats CLI (3) and Env (1)
	$node->command_checks_all(
		[
			'pg_dump', '-v', '-t', 'public.tmed', '--data-only',
			'--max-table-segment-pages=3',
			$node->connstr($dbname_chunk),
		],
		0,
		[ qr/Name: tmed; Type: TABLE DATA \(pages 0:1\); Schema: public/ ],
		[ qr// ],
		'DB pgdump.max_table_segment_pages=2 overrides CLI --max-table-segment-pages=3 and Env=1');

	# With --settings-from-database=off, DB setting (2) is ignored; CLI (3) beats Env (1)
	$node->command_checks_all(
		[
			'pg_dump', '-v', '-t', 'public.tmed', '--data-only',
			'--settings-from-database=off',
			'--max_table_segment_pages=3',
			$node->connstr($dbname_chunk),
		],
		0,
		[ qr/Name: tmed; Type: TABLE DATA \(pages 0:2\); Schema: public/ ],
		[ qr// ],
		'--settings-from-database=off ignores DB setting; CLI --max_table_segment_pages=3 overrides Env=1');

	# With PGDUMP_SETTINGS_FROM_DATABASE=off and no CLI max-table-segment-pages, Env (1) is used
	local $ENV{PGDUMP_SETTINGS_FROM_DATABASE} = 'off';
	$node->command_checks_all(
		[
			'pg_dump', '-v', '-t', 'public.tmed', '--data-only',
			$node->connstr($dbname_chunk),
		],
		0,
		[ qr/Name: tmed; Type: TABLE DATA \(pages 0:0\); Schema: public/ ],
		[ qr// ],
		'PGDUMP_SETTINGS_FROM_DATABASE=off ignores DB setting and uses Env PGDUMP_MAX_TABLE_SEGMENT_PAGES=1');

	# CLI --settings-from-database=on overrides PGDUMP_SETTINGS_FROM_DATABASE=off
	$node->command_checks_all(
		[
			'pg_dump', '-v', '-t', 'public.tmed', '--data-only',
			'--settings-from-database=on',
			$node->connstr($dbname_chunk),
		],
		0,
		[ qr/Name: tmed; Type: TABLE DATA \(pages 0:1\); Schema: public/ ],
		[ qr// ],
		'CLI --settings-from-database=on overrides PGDUMP_SETTINGS_FROM_DATABASE=off');
}
$node->safe_psql($dbname_chunk, "ALTER DATABASE $dbname_chunk RESET pgdump.max_table_segment_pages");

# 3c. chunk_size and chunk_above_size across DB, CLI, and Env (with single-setting defaulting & TOAST scaling)
# Set only pgdump.chunk_size = '16kB'; chunk_above_size defaults to 16kB.
# - tsmall (8kB <= 16kB) is NOT chunked.
# - tmed (~48kB > 16kB, no TOAST table) is chunked into 16kB / 8kB = 2 pages per segment (0:1, 2:3, ...).
# - ttoast (relbytes ~32kB, tablebytes ~300kB > 16kB): num_chunks ~ 18.75, main_bytes_per_chunk ~ 1.7kB -> clamps to 1 page per segment (0:0, 1:1, ...)!
$node->safe_psql($dbname_chunk, "ALTER DATABASE $dbname_chunk SET pgdump.chunk_size = '16kB'");
$node->command_checks_all(
	[
		'pg_dump', '-v', '-n', 'public', '--data-only',
		$node->connstr($dbname_chunk),
	],
	0,
	[
		qr/Name: tsmall; Type: TABLE DATA; Schema: public/,
		qr/Name: tmed; Type: TABLE DATA \(pages 0:1\); Schema: public/,
		qr/Name: ttoast; Type: TABLE DATA \(pages 0:0\); Schema: public/,
	],
	[ qr// ],
	'pgdump.chunk_size=16kB defaults chunk_above_size, skips tsmall, chunks tmed (2 pages), and scales ttoast for TOAST (1 page)');

# Now set pgdump.chunk_above_size = '100kB' (higher than tmed ~72kB, lower than ttoast ~300kB)
# and pgdump.chunk_size = '64kB'.
# - tmed (tablebytes ~72kB <= 100kB) is NOT chunked.
# - ttoast (tablebytes ~300kB > 100kB) IS chunked even though its main heap is only ~32kB (< 64kB), because TOAST size is included!
$node->safe_psql($dbname_chunk, "ALTER DATABASE $dbname_chunk SET pgdump.chunk_above_size = '100kB'");
$node->safe_psql($dbname_chunk, "ALTER DATABASE $dbname_chunk SET pgdump.chunk_size = '64kB'");
$node->command_checks_all(
	[
		'pg_dump', '-v', '-n', 'public', '--data-only',
		$node->connstr($dbname_chunk),
	],
	0,
	[
		qr/Name: tmed; Type: TABLE DATA; Schema: public/,
		qr/Name: ttoast; Type: TABLE DATA \(pages 0:0\); Schema: public/,
	],
	[ qr// ],
	'pgdump.chunk_above_size=100kB skips tmed (~72kB) but chunks ttoast (~300kB total with TOAST)');

$node->safe_psql($dbname_chunk, "ALTER DATABASE $dbname_chunk RESET pgdump.chunk_above_size");
$node->safe_psql($dbname_chunk, "ALTER DATABASE $dbname_chunk RESET pgdump.chunk_size");

# Test CLI and Env for chunk_above_size and chunk_size (and precedence CLI > Env)
{
	local $ENV{PGDUMP_CHUNK_ABOVE_SIZE} = '100M';
	local $ENV{PGDUMP_CHUNK_SIZE} = '100M';

	$node->command_checks_all(
		[
			'pg_dump', '-v', '-t', 'public.tmed', '--data-only',
			'--chunk_above_size=16kB', '--chunk_size=24kB',
			$node->connstr($dbname_chunk),
		],
		0,
		[ qr/Name: tmed; Type: TABLE DATA \(pages 0:2\); Schema: public/ ],
		[ qr// ],
		'CLI --chunk_above_size=16kB --chunk_size=24kB overrides PGDUMP_CHUNK_* env vars');
}

# Test setting ONLY pgdump.chunk_above_size (chunk_size defaults to chunk_above_size)
$node->safe_psql($dbname_chunk, "ALTER DATABASE $dbname_chunk SET pgdump.chunk_above_size = '16k'");
$node->command_checks_all(
	[
		'pg_dump', '-v', '-t', 'public.tmed', '--data-only',
		$node->connstr($dbname_chunk),
	],
	0,
	[ qr/Name: tmed; Type: TABLE DATA \(pages 0:1\); Schema: public/ ],
	[ qr// ],
	'pgdump.chunk_above_size alone defaults chunk_size to chunk_above_size');

# Test proportional multi-page TOAST scaling (seg_pages = 2, i.e. pages 0:1)
# For ttoast (4 main heap pages), choosing chunk_size = 5/8 of pg_table_size(ttoast) yields:
# num_chunks = 8/5 = 1.6, main_pages_per_chunk = 4 / 1.6 = 2.5 -> floor(2.5) = 2 pages
my $ttoast_two_page_kb = $node->safe_psql($dbname_chunk,
	"SELECT (pg_table_size('public.ttoast') * 5 / 8 / 1024)::int");
$node->safe_psql($dbname_chunk, "ALTER DATABASE $dbname_chunk SET pgdump.chunk_above_size = '${ttoast_two_page_kb}kB'");
$node->command_checks_all(
	[
		'pg_dump', '-v', '-t', 'public.ttoast', '--data-only',
		$node->connstr($dbname_chunk),
	],
	0,
	[
		qr/Name: ttoast; Type: TABLE DATA \(pages 0:1\); Schema: public/,
		qr/Name: ttoast; Type: TABLE DATA \(pages 2:4294967295\); Schema: public/,
	],
	[ qr// ],
	'TOAST scaling formula computes proportional multi-page segment size (2 pages)');

# Verify --schema-only succeeds without TABLE DATA entries
$node->command_checks_all(
	[
		'pg_dump', '-v', '-t', 'public.ttoast', '--schema-only',
		$node->connstr($dbname_chunk),
	],
	0,
	[ qr/Name: ttoast; Type: TABLE; Schema: public/ ],
	[ qr// ],
	'--schema-only with DB chunking settings succeeds without chunking table data');
$node->safe_psql($dbname_chunk, "ALTER DATABASE $dbname_chunk RESET pgdump.chunk_above_size");

# 3d. table_chunk_sizes precedence:
# (1) Within a single source: schema-qualified beats unqualified, and last duplicate wins
$node->safe_psql($dbname_chunk, "ALTER DATABASE $dbname_chunk SET pgdump.max_table_segment_pages = '100'");
$node->safe_psql($dbname_chunk, "ALTER DATABASE $dbname_chunk SET pgdump.table_chunk_sizes = 'shared_tbl:100M, s1.shared_tbl:16kB, tmed:100M, tmed:24kB'");
$node->command_checks_all(
	[
		'pg_dump', '-v', '--data-only',
		$node->connstr($dbname_chunk),
	],
	0,
	[
		qr/Name: shared_tbl; Type: TABLE DATA \(pages 0:1\); Schema: s1/,
		qr/Name: shared_tbl; Type: TABLE DATA; Schema: s2/,
		qr/Name: tmed; Type: TABLE DATA \(pages 0:2\); Schema: public/,
	],
	[ qr// ],
	'pgdump.table_chunk_sizes precedence, schema-qualified override, and last-duplicate-wins');

# (2) Per-table merging across 1. Database > 2. Command Line > 3. Environment:
# - Env sets PGDUMP_TABLE_CHUNK_SIZES = 's1.shared_tbl:16kB, s2.shared_tbl:16kB, tmed:16kB'
# - CLI sets --table-chunk-sizes = 's2.shared_tbl:24kB, tmed:100M'
# - DB sets pgdump.table_chunk_sizes = 'tmed:32kB'
# Expected when settings-from-database=on:
#   * s1.shared_tbl -> 16kB (2 pages: 0:1) from Env (not in DB or CLI)
#   * s2.shared_tbl -> 24kB (3 pages: 0:2) from CLI (overrides Env; not in DB)
#   * public.tmed   -> 32kB (4 pages: 0:3) from DB (overrides CLI and Env)
# Expected when --settings-from-database=off:
#   * s1.shared_tbl -> 16kB (2 pages: 0:1) from Env
#   * s2.shared_tbl -> 24kB (3 pages: 0:2) from CLI
#   * public.tmed   -> 100M (not chunked) from CLI (DB ignored)
$node->safe_psql($dbname_chunk, "ALTER DATABASE $dbname_chunk RESET pgdump.max_table_segment_pages");
$node->safe_psql($dbname_chunk, "ALTER DATABASE $dbname_chunk SET pgdump.table_chunk_sizes = 'tmed:32kB'");
{
	local $ENV{PGDUMP_TABLE_CHUNK_SIZES} = 's1.shared_tbl:16kB, s2.shared_tbl:16kB, tmed:16kB';

	$node->command_checks_all(
		[
			'pg_dump', '-v', '--data-only',
			'--table-chunk-sizes=s2.shared_tbl:24kB, tmed:100M',
			$node->connstr($dbname_chunk),
		],
		0,
		[
			qr/Name: shared_tbl; Type: TABLE DATA \(pages 0:1\); Schema: s1/,
			qr/Name: shared_tbl; Type: TABLE DATA \(pages 0:2\); Schema: s2/,
			qr/Name: tmed; Type: TABLE DATA \(pages 0:3\); Schema: public/,
		],
		[ qr// ],
		'per-table table_chunk_sizes merges 1. DB (tmed:32kB) > 2. CLI (s2.shared_tbl:24kB) > 3. Env (s1.shared_tbl:16kB)');

	$node->command_checks_all(
		[
			'pg_dump', '-v', '--data-only',
			'--settings-from-database=off',
			'--table-chunk-sizes=s2.shared_tbl:24kB, tmed:100M',
			$node->connstr($dbname_chunk),
		],
		0,
		[
			qr/Name: shared_tbl; Type: TABLE DATA \(pages 0:1\); Schema: s1/,
			qr/Name: shared_tbl; Type: TABLE DATA \(pages 0:2\); Schema: s2/,
			qr/Name: tmed; Type: TABLE DATA; Schema: public/,
		],
		[ qr// ],
		'per-table table_chunk_sizes with --settings-from-database=off merges 2. CLI > 3. Env and ignores DB');

	# (3) Option precedence when all 4 chunking settings are active:
	# table_chunk_sizes (tmed -> 32kB = 4 pages [0:3], s2.shared_tbl -> 24kB = 3 pages [0:2], s1.shared_tbl -> 16kB = 2 pages [0:1])
	# overrides max-table-segment-pages (3 pages [0:2]), which overrides chunk_size (8kB = 1 page) for non-overridden tables (ttoast -> 3 pages [0:2])
	$node->command_checks_all(
		[
			'pg_dump', '-v', '--data-only',
			'--table-chunk-sizes=s2.shared_tbl:24kB, tmed:100M',
			'--max-table-segment-pages=3',
			'--chunk-above-size=8kB',
			'--chunk-size=8kB',
			$node->connstr($dbname_chunk),
		],
		0,
		[
			qr/Name: shared_tbl; Type: TABLE DATA \(pages 0:1\); Schema: s1/,
			qr/Name: shared_tbl; Type: TABLE DATA \(pages 0:2\); Schema: s2/,
			qr/Name: tmed; Type: TABLE DATA \(pages 0:3\); Schema: public/,
			qr/Name: ttoast; Type: TABLE DATA \(pages 0:2\); Schema: public/,
		],
		[ qr// ],
		'option hierarchy: table_chunk_sizes > max-table-segment-pages > chunk_size');
}

# Restore DB setting for parallel dump + restore test
$node->safe_psql($dbname_chunk, "ALTER DATABASE $dbname_chunk SET pgdump.max_table_segment_pages = '100'");
$node->safe_psql($dbname_chunk, "ALTER DATABASE $dbname_chunk SET pgdump.table_chunk_sizes = 'shared_tbl:100M, s1.shared_tbl:16kB, tmed:100M, tmed:24kB'");

# 3e. Parallel directory dump + restore with default settings-from-database=on
{
	$node->command_ok(
		[
			'pg_dump',
			'--format' => 'directory',
			'--no-sync',
			'--jobs' => 2,
			'--file' => "$backupdir/dump_from_db",
			$node->connstr($dbname_chunk),
		],
		'parallel directory dump with database-configured pgdump.* settings');

	$node->command_ok(
		[
			'pg_restore', '--verbose',
			'--dbname' => $node->connstr($dbname_chunk_restore),
			'--jobs' => 2,
			"$backupdir/dump_from_db",
		],
		'parallel restore of database-configured chunked dump');

	for my $tbl ('public.tsmall', 'public.tmed', 'public.ttoast', 's1.shared_tbl', 's2.shared_tbl')
	{
		my $q = "SELECT '$tbl', sum(hashtext(t::text)), count(*) FROM $tbl AS t";
		my $orig = $node->safe_psql($dbname_chunk, $q);
		my $rest = $node->safe_psql($dbname_chunk_restore, $q);
		is($rest, $orig, "Hash check for $tbl after database-configured chunked dump/restore");
	}
}

# 3f. pg_dumpall with database-configured settings and CLI forwarding
$node->command_checks_all(
	[
		'pg_dumpall', '--verbose', '--data-only',
		'--exclude-database=postgres',
		'--exclude-database=regression_dest*',
		'--exclude-database=regression_db_chunk_restore',
		'--database' => $node->connstr($dbname_chunk),
	],
	0,
	[ qr/Name: shared_tbl; Type: TABLE DATA \(pages 0:1\); Schema: s1/ ],
	[ qr// ],
	'pg_dumpall uses per-database pgdump.* settings by default');

$node->command_checks_all(
	[
		'pg_dumpall', '--verbose', '--data-only',
		'--settings-from-database=off',
		'--table-chunk-sizes=s1.shared_tbl:24kB',
		'--exclude-database=postgres',
		'--exclude-database=regression_dest*',
		'--exclude-database=regression_db_chunk_restore',
		'--database' => $node->connstr($dbname_chunk),
	],
	0,
	[ qr/Name: shared_tbl; Type: TABLE DATA \(pages 0:2\); Schema: s1/ ],
	[ qr// ],
	'pg_dumpall forwards --settings-from-database=off and --table-chunk-sizes to pg_dump');

# 3g. Unit suffixes (k/kB, M/MB/default M, G/GB, T/TB) and hyphenated GUC aliases
{
	local $ENV{PGOPTIONS} = '-c pgdump.table-chunk-sizes=s1.shared_tbl:16k,s2.shared_tbl:1,tmed:1G,tsmall:1TB';
	$node->command_checks_all(
		[
			'pg_dump', '-v', '--data-only',
			$node->connstr($dbname_chunk),
		],
		0,
		[
			qr/Name: shared_tbl; Type: TABLE DATA \(pages 0:1\); Schema: s1/,
			qr/Name: shared_tbl; Type: TABLE DATA; Schema: s2/,
			qr/Name: tmed; Type: TABLE DATA; Schema: public/,
		],
		[ qr// ],
		'unit suffixes k, unitless default M, G, TB work in pgdump.table-chunk-sizes');
}

# 3h. --binary-upgrade compatibility:
# Fails when DB has active chunking settings and settings-from-database=on,
# but succeeds when --settings-from-database=off!
$node->command_fails_like(
	[
		'pg_dump', '--binary-upgrade', '--schema-only',
		$node->connstr($dbname_chunk),
	],
	qr/\Qpg_dump: error: table chunking options and --binary-upgrade cannot be used together\E/,
	'--binary-upgrade fails when DB has pgdump.* chunking settings and settings-from-database=on');

$node->command_ok(
	[
		'pg_dump', '--binary-upgrade', '--schema-only',
		'--settings-from-database=off',
		'--file' => "$backupdir/dump_bin_upgrade.sql",
		$node->connstr($dbname_chunk),
	],
	'--binary-upgrade succeeds when --settings-from-database=off');

# 3i. Invalid in-database GUC values fail clearly when settings-from-database is on,
# and are ignored when --settings-from-database=off
for my $bad_chunk_size ('10XB', '0', '0MB', '-5M', '999999999TB', '')
{
	next if $bad_chunk_size eq '';
	local $ENV{PGOPTIONS} = "-c pgdump.chunk_size=$bad_chunk_size";
	$node->command_fails_like(
		[
			'pg_dump',
			$node->connstr($dbname_chunk),
		],
		qr/\Qpg_dump: error: invalid value for pgdump.chunk_size: "$bad_chunk_size"\E/,
		"invalid pgdump.chunk_size='$bad_chunk_size' fails");
}

for my $bad_pages ('0', '4294967295', 'not_a_number', 'from-database')
{
	local $ENV{PGOPTIONS} = "-c pgdump.max_table_segment_pages=$bad_pages";
	$node->command_fails_like(
		[
			'pg_dump',
			$node->connstr($dbname_chunk),
		],
		qr/pg_dump: error: .*pgdump\.max-table-segment-pages/,
		"invalid pgdump.max_table_segment_pages='$bad_pages' fails");
}

for my $bad_above ('0', '10XB', '-1M')
{
	local $ENV{PGOPTIONS} = "-c pgdump.chunk_above_size=$bad_above";
	$node->command_fails_like(
		[
			'pg_dump',
			$node->connstr($dbname_chunk),
		],
		qr/\Qpg_dump: error: invalid value for pgdump.chunk_above_size: "$bad_above"\E/,
		"invalid pgdump.chunk_above_size='$bad_above' fails");
}

for my $bad_spec ('missing_colon', 'a.b.c:10M', '.tbl:10M', 's1.:10M', ':10M', 'tbl:', 'tbl:0kB', 'tbl:10XB', 't1:10M,')
{
	local $ENV{PGOPTIONS} = "-c pgdump.table_chunk_sizes=$bad_spec";
	$node->command_fails_like(
		[
			'pg_dump',
			$node->connstr($dbname_chunk),
		],
		qr/\Qpg_dump: error: invalid value for pgdump.table_chunk_sizes:\E/,
		"invalid pgdump.table_chunk_sizes='$bad_spec' fails");
}

{
	local $ENV{PGOPTIONS} = "-c pgdump.chunk_size=invalid_ignored";
	$node->command_ok(
		[
			'pg_dump', '--schema-only', '--settings-from-database=off',
			'--file' => "$backupdir/dump_ignored_guc.sql",
			$node->connstr($dbname_chunk),
		],
		'invalid pgdump.chunk_size in DB is ignored when --settings-from-database=off');
}

done_testing();
