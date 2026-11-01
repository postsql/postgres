
# Copyright (c) 2026, PostgreSQL Global Development Group

# Logical replication by tuple identifier into a table that stores the
# ".rowid" column only in its index (storage parameter index_only_rowid).
#
# Since ".rowid" reads as null from the heap of such a table, the tests read
# it through its index: each publisher row is looked up on the subscriber by
# its tuple identifier, using an index scan.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node_publisher = PostgreSQL::Test::Cluster->new('publisher');
$node_publisher->init(allows_streaming => 'logical');
$node_publisher->append_conf('postgresql.conf',
	"logical_decoding_work_mem = 64kB\nautovacuum = off");
$node_publisher->start;

# The subscriber logs logical information too, to check that changes of the
# index-only table can be logged without its key.
my $node_subscriber = PostgreSQL::Test::Cluster->new('subscriber');
$node_subscriber->init(allows_streaming => 'logical');
$node_subscriber->append_conf('postgresql.conf',
	"autovacuum = off\nlogical_replication_index_only_rowid = on");
$node_subscriber->start;

my $publisher_connstr = $node_publisher->connstr . ' dbname=postgres';
my $big = "(SELECT string_agg(md5(g::text), '') FROM generate_series(1, 3000) g)";

# Two tables receiving the same changes: one stores ".rowid" normally, the
# other only in its index.
foreach my $tab (qw(tab_io tab_st))
{
	$node_publisher->safe_psql(
		'postgres', qq(
		CREATE TABLE $tab (id int, val text, big text);
		ALTER TABLE $tab REPLICA IDENTITY ROWID;
		INSERT INTO $tab SELECT g, 'initial ' || g, NULL FROM generate_series(1, 2000) g;
		UPDATE $tab SET big = $big WHERE id = 1;
	));
}
$node_publisher->safe_psql('postgres',
	"CREATE PUBLICATION tap_pub FOR TABLE tab_io, tab_st");

$node_subscriber->safe_psql(
	'postgres', qq(
	CREATE TABLE tab_io (id int, val text, big text, ".rowid" tid PRIMARY KEY)
		WITH (index_only_rowid = true);
	CREATE TABLE tab_st (id int, val text, big text, ".rowid" tid PRIMARY KEY);
));
$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION tap_sub CONNECTION '$publisher_connstr' PUBLICATION tap_pub WITH (streaming = parallel)"
);
$node_subscriber->wait_for_subscription_sync($node_publisher, 'tap_sub');

# Compare both tables with the publisher.  For tab_io, find each publisher
# row on the subscriber through the index on ".rowid".
sub check_tabs
{
	my ($msg) = @_;

	$node_publisher->wait_for_catchup('tap_sub');

	foreach my $tab (qw(tab_io tab_st))
	{
		my $pub = $node_publisher->safe_psql('postgres',
			"SELECT ctid, id, val, md5(big) FROM $tab ORDER BY id");
		my $tids = $node_publisher->safe_psql('postgres',
			"SELECT string_agg(quote_literal(ctid::text), ',') FROM $tab");
		my $sub = $node_subscriber->safe_psql(
			'postgres', qq(
			SET enable_seqscan = off;
			SET enable_bitmapscan = off;
			SET enable_hashjoin = off;
			SET enable_mergejoin = off;
			SELECT v.tid, t.id, t.val, md5(t.big)
			FROM unnest(ARRAY[$tids]::tid[]) AS v(tid)
				JOIN $tab t ON t.".rowid" = v.tid
			ORDER BY t.id;
		));
		is($sub, $pub, "$msg: $tab matches the publisher");
	}
	return;
}

# Number of rows of tab_io whose ".rowid" is stored in the table itself
sub stored_rowids
{
	return $node_subscriber->safe_psql(
		'postgres', qq(
		SET enable_indexscan = off;
		SET enable_indexonlyscan = off;
		SET enable_bitmapscan = off;
		SELECT count(r) FROM (SELECT ".rowid" AS r FROM tab_io OFFSET 0) s;
	));
}

check_tabs('initial synchronization');
is(stored_rowids(), '0',
	'initial synchronization stored no tuple identifiers in tab_io');

$node_publisher->safe_psql(
	'postgres', qq(
	INSERT INTO tab_io VALUES (5000, 'single', NULL);
	INSERT INTO tab_st VALUES (5000, 'single', NULL);
	INSERT INTO tab_io SELECT g, 'multi ' || g, NULL FROM generate_series(5001, 5100) g;
	INSERT INTO tab_st SELECT g, 'multi ' || g, NULL FROM generate_series(5001, 5100) g;
	UPDATE tab_io SET val = 'same page ' || id WHERE id BETWEEN 2 AND 50;
	UPDATE tab_st SET val = 'same page ' || id WHERE id BETWEEN 2 AND 50;
	UPDATE tab_io SET val = repeat('w', 1500) WHERE id BETWEEN 60 AND 64;
	UPDATE tab_st SET val = repeat('w', 1500) WHERE id BETWEEN 60 AND 64;
	UPDATE tab_io SET val = 'toast unchanged' WHERE id = 1;
	UPDATE tab_st SET val = 'toast unchanged' WHERE id = 1;
	DELETE FROM tab_io WHERE id % 13 = 0;
	DELETE FROM tab_st WHERE id % 13 = 0;
));
check_tabs('INSERT, UPDATE and DELETE');

$node_publisher->safe_psql(
	'postgres', qq(
	BEGIN;
	INSERT INTO tab_io SELECT g, 'large ' || g, NULL FROM generate_series(10000, 12000) g;
	SAVEPOINT s1;
	DELETE FROM tab_io;
	ROLLBACK TO s1;
	UPDATE tab_io SET val = 'large upd ' || id WHERE id % 7 = 0;
	COMMIT;
	INSERT INTO tab_st SELECT g, 'large ' || g, NULL FROM generate_series(10000, 12000) g;
	UPDATE tab_st SET val = 'large upd ' || id WHERE id % 7 = 0;
));
check_tabs('large transaction');
is(stored_rowids(), '0', 'no tuple identifiers stored in tab_io');

my $result = $node_subscriber->safe_psql('postgres',
	"SELECT md5(big) = md5($big) FROM tab_io WHERE id = 1");
is($result, 't', 'unchanged out-of-line value preserved');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT pg_relation_size('tab_io') < pg_relation_size('tab_st')");
is($result, 't', 'tab_io is smaller than tab_st');

# Nothing that would build ".rowid" from the heap is allowed
my %refused = (
	'REINDEX TABLE tab_io' => qr/cannot build index "tab_io_pkey" on table "tab_io"/,
	'CREATE INDEX ON tab_io (".rowid")' => qr/cannot build index .* on table "tab_io"/,
	'VACUUM FULL tab_io' => qr/cannot execute VACUUM FULL on table "tab_io"/,
	'CLUSTER tab_io USING tab_io_pkey' => qr/cannot execute CLUSTER on table "tab_io"/,
	'ALTER TABLE tab_io ALTER COLUMN id TYPE bigint' =>
	  qr/cannot execute ALTER TABLE on table "tab_io"/,
	'ALTER TABLE tab_io SET (index_only_rowid = false)' =>
	  qr/cannot change storage parameter "index_only_rowid" of table "tab_io" because it is not empty/,
	"UPDATE tab_io SET val = 'local'" =>
	  qr/cannot update or delete rows of table "tab_io"/,);
foreach my $cmd (sort keys %refused)
{
	my ($ret, $stdout, $stderr) = $node_subscriber->psql('postgres', $cmd);
	like($stderr, $refused{$cmd}, "refused: $cmd");
}

# Other indexes can be built
$node_subscriber->safe_psql('postgres', "CREATE INDEX ON tab_io (id)");

# Publishing UPDATE or DELETE of the table is refused, as its key is not
# stored in the table
my ($ret, $stdout, $stderr) = $node_subscriber->psql(
	'postgres', qq(
	CREATE PUBLICATION sub_pub FOR TABLE tab_io;
	SET session_replication_role = replica;
	DELETE FROM tab_io WHERE id = 3;
));
like(
	$stderr,
	qr/cannot delete from table "tab_io" because it publishes deletes/,
	'publishing DELETE of an index-only table refused');
$node_subscriber->safe_psql('postgres', "DROP PUBLICATION sub_pub");

# With the server setting off, new rows store ".rowid" normally, and the
# restrictions remain.
$node_subscriber->safe_psql('postgres',
	"ALTER SYSTEM SET logical_replication_index_only_rowid = off");
$node_subscriber->restart;
$node_publisher->safe_psql(
	'postgres', qq(
	INSERT INTO tab_io SELECT g, 'stored ' || g, NULL FROM generate_series(20000, 20009) g;
	INSERT INTO tab_st SELECT g, 'stored ' || g, NULL FROM generate_series(20000, 20009) g;
	UPDATE tab_io SET val = 'stored upd' WHERE id = 4;
	UPDATE tab_st SET val = 'stored upd' WHERE id = 4;
));
check_tabs('server setting off');
is(stored_rowids(), '11', 'new row versions store their tuple identifier');
($ret, $stdout, $stderr) =
  $node_subscriber->psql('postgres', 'REINDEX TABLE tab_io');
like($stderr, qr/cannot build index/,
	'restrictions remain with the server setting off');

# An empty table can be changed freely
$node_subscriber->safe_psql(
	'postgres', qq(
	CREATE TABLE tab_empty (id int, ".rowid" tid PRIMARY KEY) WITH (index_only_rowid = true);
	REINDEX TABLE tab_empty;
	ALTER TABLE tab_empty SET (index_only_rowid = false);
	DROP TABLE tab_empty;
));
pass('empty index-only table can be reindexed and reconfigured');

# Finishing the migration: drop the key and the column (without rewrite);
# afterwards, the table is an ordinary table.
$node_subscriber->safe_psql(
	'postgres', qq(
	DROP SUBSCRIPTION tap_sub;
	ALTER TABLE tab_io DROP CONSTRAINT tab_io_pkey;
	ALTER TABLE tab_io DROP COLUMN ".rowid";
	UPDATE tab_io SET val = 'local' WHERE id = 3;
	VACUUM FULL tab_io;
));
$result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*), count(*) FILTER (WHERE val = 'local') FROM tab_io");
my $expected = $node_publisher->safe_psql('postgres',
	"SELECT count(*), 1 FROM tab_io");
is($result, $expected, 'table is an ordinary table after the migration');

foreach my $node ($node_publisher, $node_subscriber)
{
	my $log = slurp_file($node->logfile);
	unlike($log, qr/TRAP:|PANIC:|WARNING:  .*leak/,
		$node->name . ': no assertion failures or leaks');
}

$node_subscriber->stop('fast');
$node_publisher->stop('fast');

done_testing();
