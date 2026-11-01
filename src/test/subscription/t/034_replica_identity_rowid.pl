
# Copyright (c) 2026, PostgreSQL Global Development Group

# Logical replication of tables without a key by tuple identifier
# (REPLICA IDENTITY ROWID).
#
# The tests compare complete table contents after every step, including
# the subscriber's ".rowid" column against the publisher's ctid, and wait
# for catchup rather than for fixed periods of time.
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

my $node_subscriber = PostgreSQL::Test::Cluster->new('subscriber');
$node_subscriber->init;
$node_subscriber->append_conf('postgresql.conf', "autovacuum = off");
$node_subscriber->start;

my $publisher_connstr = $node_publisher->connstr . ' dbname=postgres';

# A value that is stored out of line (and does not compress well)
my $big = "(SELECT string_agg(md5(g::text), '') FROM generate_series(1, 3000) g)";

$node_publisher->safe_psql(
	'postgres', qq(
	CREATE TABLE tab_rowid (id int, val text, big text);
	ALTER TABLE tab_rowid REPLICA IDENTITY ROWID;
	INSERT INTO tab_rowid SELECT g, 'initial ' || g, NULL FROM generate_series(1, 20) g;
	UPDATE tab_rowid SET big = $big WHERE id = 1;
	DELETE FROM tab_rowid WHERE id = 2;
	CREATE PUBLICATION tap_pub FOR TABLE tab_rowid;
));

$node_subscriber->safe_psql(
	'postgres', qq(
	CREATE TABLE tab_rowid (id int, val text, big text, ".rowid" tid PRIMARY KEY);
	CREATE TABLE tab_rowid_nonempty (id int, ".rowid" tid PRIMARY KEY);
	INSERT INTO tab_rowid_nonempty VALUES (0, '(0,1)');
));

# Compare the table on both nodes, including the tuple identifiers
sub check_tab
{
	my ($table, $msg) = @_;

	$node_publisher->wait_for_catchup('tap_sub');

	my $pub = $node_publisher->safe_psql('postgres',
		"SELECT ctid, id, val, md5(big) FROM $table ORDER BY id");
	my $sub = $node_subscriber->safe_psql('postgres',
		"SELECT \".rowid\", id, val, md5(big) FROM $table ORDER BY id");
	is($sub, $pub, "$msg: contents and tuple identifiers match");
	return;
}

$node_subscriber->safe_psql('postgres',
	"CREATE SUBSCRIPTION tap_sub CONNECTION '$publisher_connstr' PUBLICATION tap_pub WITH (streaming = parallel)"
);
$node_subscriber->wait_for_subscription_sync($node_publisher, 'tap_sub');
check_tab('tab_rowid', 'initial synchronization');

my $result = $node_subscriber->safe_psql('postgres',
	"SELECT count(*), count(big) FROM tab_rowid");
is($result, '19|1', 'initial synchronization copied all rows');

# Single and multi-row inserts, HOT updates, updates moving rows to another
# page, an update leaving the out-of-line value unchanged, and deletes
$node_publisher->safe_psql(
	'postgres', qq(
	INSERT INTO tab_rowid VALUES (100, 'single', NULL);
	INSERT INTO tab_rowid SELECT g, 'multi ' || g, NULL FROM generate_series(101, 110) g;
	UPDATE tab_rowid SET val = 'hot ' || id WHERE id BETWEEN 3 AND 5;
	UPDATE tab_rowid SET val = repeat('w', 1500) WHERE id BETWEEN 6 AND 10;
	UPDATE tab_rowid SET val = 'toast unchanged' WHERE id = 1;
	DELETE FROM tab_rowid WHERE id IN (11, 12, 100);
));
check_tab('tab_rowid', 'INSERT, UPDATE and DELETE');

$result = $node_subscriber->safe_psql('postgres',
	"SELECT md5(big) = md5($big) FROM tab_rowid WHERE id = 1");
is($result, 't', 'unchanged out-of-line value preserved');

# Pruning makes tuple identifiers of deleted rows reusable; new rows reusing
# them must not be confused with the deleted ones.
$node_publisher->safe_psql('postgres', "VACUUM tab_rowid");
$node_publisher->safe_psql('postgres',
	"INSERT INTO tab_rowid SELECT g, 'reuse ' || g, NULL FROM generate_series(200, 240) g"
);
$result = $node_publisher->safe_psql('postgres',
	"SELECT count(*) > 0 FROM tab_rowid WHERE id >= 200 AND ctid < '(1,0)'");
is($result, 't', 'new rows reuse space of deleted rows');
check_tab('tab_rowid', 'reused tuple identifiers');

# A large transaction (streamed to the subscriber) with a rolled back
# subtransaction
$node_publisher->safe_psql(
	'postgres', qq(
	BEGIN;
	INSERT INTO tab_rowid SELECT g, 'large ' || g, NULL FROM generate_series(1000, 4000) g;
	SAVEPOINT s1;
	UPDATE tab_rowid SET val = 'rolled back' WHERE id >= 1000;
	DELETE FROM tab_rowid WHERE id < 1000;
	ROLLBACK TO s1;
	UPDATE tab_rowid SET val = 'large upd ' || id WHERE id % 7 = 0;
	DELETE FROM tab_rowid WHERE id % 11 = 0;
	COMMIT;
));
check_tab('tab_rowid', 'large transaction with rolled back subtransaction');

# Binary transfer
$node_subscriber->safe_psql('postgres',
	"ALTER SUBSCRIPTION tap_sub SET (binary = true)");
$node_publisher->safe_psql(
	'postgres', qq(
	INSERT INTO tab_rowid VALUES (5000, 'binary', NULL);
	UPDATE tab_rowid SET val = 'binary upd' WHERE id IN (5000, 3);
	DELETE FROM tab_rowid WHERE id = 4;
));
check_tab('tab_rowid', 'binary transfer');

# The publisher refuses to rewrite the table, which would change the tuple
# identifiers.
my ($ret, $stdout, $stderr) =
  $node_publisher->psql('postgres', "VACUUM FULL tab_rowid");
like(
	$stderr,
	qr/cannot execute VACUUM FULL on table "tab_rowid"/,
	'VACUUM FULL refused on publisher');
($ret, $stdout, $stderr) = $node_publisher->psql('postgres',
	"ALTER TABLE tab_rowid ALTER COLUMN id TYPE bigint");
like(
	$stderr,
	qr/cannot execute ALTER TABLE on table "tab_rowid"/,
	'rewriting ALTER TABLE refused on publisher');

# The subscriber refuses local changes of the target table
($ret, $stdout, $stderr) =
  $node_subscriber->psql('postgres', "UPDATE tab_rowid SET val = 'local'");
like(
	$stderr,
	qr/cannot update or delete rows of table "tab_rowid"/,
	'local UPDATE refused on subscriber');
($ret, $stdout, $stderr) =
  $node_subscriber->psql('postgres', "DELETE FROM tab_rowid");
like(
	$stderr,
	qr/cannot update or delete rows of table "tab_rowid"/,
	'local DELETE refused on subscriber');
$node_subscriber->safe_psql(
	'postgres', qq(
	CREATE ROLE regress_rowid_user;
	GRANT ALL ON tab_rowid TO regress_rowid_user;
));
($ret, $stdout, $stderr) = $node_subscriber->psql(
	'postgres', qq(
	SET ROLE regress_rowid_user;
	SET session_replication_role = replica;
));
like(
	$stderr,
	qr/permission denied to set parameter "session_replication_role"/,
	'unprivileged user cannot bypass the restriction');
$result = $node_subscriber->safe_psql(
	'postgres', qq(
	BEGIN;
	SET LOCAL session_replication_role = replica;
	DELETE FROM tab_rowid WHERE id = 3;
	ROLLBACK;
	SELECT count(*) FROM tab_rowid WHERE id = 3;
));
is($result, '1', 'superuser can override with session_replication_role');
check_tab('tab_rowid', 'after refused local changes');

# Initial synchronization into a non-empty table fails, until it is emptied
$node_publisher->safe_psql(
	'postgres', qq(
	CREATE TABLE tab_rowid_nonempty (id int);
	ALTER TABLE tab_rowid_nonempty REPLICA IDENTITY ROWID;
	INSERT INTO tab_rowid_nonempty VALUES (1), (2);
	ALTER PUBLICATION tap_pub ADD TABLE tab_rowid_nonempty;
));
my $offset = -s $node_subscriber->logfile;
$node_subscriber->safe_psql('postgres',
	"ALTER SUBSCRIPTION tap_sub REFRESH PUBLICATION");
$node_subscriber->wait_for_log(
	qr/cannot synchronize table "public.tab_rowid_nonempty" because it is not empty/,
	$offset);
$node_subscriber->safe_psql(
	'postgres', qq(
	BEGIN;
	SET LOCAL session_replication_role = replica;
	DELETE FROM tab_rowid_nonempty;
	COMMIT;
));
$node_subscriber->wait_for_subscription_sync($node_publisher, 'tap_sub');
$node_publisher->wait_for_catchup('tap_sub');
$result = $node_publisher->safe_psql('postgres',
	"SELECT ctid, id FROM tab_rowid_nonempty ORDER BY id");
is( $node_subscriber->safe_psql(
		'postgres', "SELECT \".rowid\", id FROM tab_rowid_nonempty ORDER BY id"),
	$result,
	'synchronization succeeds after emptying the table');

# After the migration, the key and the column can be dropped without
# rewriting the table, and local changes are possible again.
$node_subscriber->safe_psql(
	'postgres', qq(
	DROP SUBSCRIPTION tap_sub;
	ALTER TABLE tab_rowid DROP CONSTRAINT tab_rowid_pkey;
	ALTER TABLE tab_rowid DROP COLUMN ".rowid";
	UPDATE tab_rowid SET val = 'local' WHERE id = 3;
));
$result = $node_subscriber->safe_psql('postgres',
	"SELECT val FROM tab_rowid WHERE id = 3");
is($result, 'local', 'table is writable after the migration');

# pg_dump preserves the replica identity
command_like(
	[
		'pg_dump', '--schema-only', '--table' => 'tab_rowid',
		'--dbname' => $node_publisher->connstr('postgres'),
	],
	qr/ALTER TABLE ONLY public\.tab_rowid REPLICA IDENTITY ROWID;/,
	'pg_dump dumps REPLICA IDENTITY ROWID');

# No crashes or resource leaks
foreach my $node ($node_publisher, $node_subscriber)
{
	my $log = slurp_file($node->logfile);
	unlike($log, qr/TRAP:|PANIC:|WARNING:  .*leak/,
		$node->name . ': no assertion failures or leaks');
}

$node_subscriber->stop('fast');
$node_publisher->stop('fast');

done_testing();
