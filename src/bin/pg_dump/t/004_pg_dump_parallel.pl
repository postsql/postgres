
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

done_testing();
