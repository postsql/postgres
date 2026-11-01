# Copyright (c) 2026, PostgreSQL Global Development Group

# Test libpq Primary/RO connection association and automatic WAIT FOR COMMIT LSN

use strict;
use warnings FATAL => 'all';

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

$SIG{ALRM} = sub { die "Test timed out after 120 seconds\n"; };
alarm(120);

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->start;

$primary->safe_psql('postgres', q{
	CREATE TABLE wait_lsn_tbl(id int PRIMARY KEY);
	CREATE ROLE regress_wait_lsn_unpriv NOLOGIN;
	GRANT SELECT ON wait_lsn_tbl TO regress_wait_lsn_unpriv;
});

$primary->backup('my_backup');

my $standby1 = PostgreSQL::Test::Cluster->new('standby1');
$standby1->init_from_backup($primary, 'my_backup', has_streaming => 1);
$standby1->start;

my $standby2 = PostgreSQL::Test::Cluster->new('standby2');
$standby2->init_from_backup($primary, 'my_backup', has_streaming => 1);
$standby2->start;

$primary->wait_for_catchup($standby1);
$primary->wait_for_catchup($standby2);

my ($out, $err) = run_command([
	'libpq_testclient', '--wait-for-commit-lsn',
	$primary->connstr('postgres'),
	$standby1->connstr('postgres'),
	$standby2->connstr('postgres'),
]);
is($err, '', 'no stderr from libpq_testclient --wait-for-commit-lsn');
is($out, 'wait_for_commit_lsn_ok',
	'Primary/RO association, set/unset WAIT FOR COMMIT LSN, error isolation, and NOTIFY fast path passed');

# Verify standby1 log recorded fastpath_no_wait_108 with zero WAIT FOR LSN in that window
my $s1_log = slurp_file($standby1->logfile);
my ($fastpath_window) = ($s1_log =~ /(fastpath_begin_108.*fastpath_no_wait_108)/s);
ok(defined($fastpath_window),
	'standby1 logged the post-NOTIFY fast-path query window');
unlike($fastpath_window // '', qr/WAIT FOR LSN/,
	'NOTIFY-advanced satisfied_lsn bypassed WAIT FOR LSN on standby1');

# Verify zero engine/resource leak warnings across all three nodes
for my $node ($primary, $standby1, $standby2)
{
	my $log = slurp_file($node->logfile);
	unlike($log, qr/WARNING:.*(?:leak|reference|WaitLSN)/i,
		$node->name . ' server log is free of leak warnings');
}

$standby2->stop;
$standby1->stop;
$primary->stop;

done_testing();
