# Copyright (c) 2026, PostgreSQL Global Development Group

# Test libpq extended ReadyForQuery parsing, accessors, and malformed frame handling

use strict;
use warnings FATAL => 'all';

use IO::Socket::INET;
use POSIX qw(_exit);
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

$SIG{ALRM} = sub { die "Test timed out after 90 seconds\n"; };
alarm(90);

# 1. Primary + standby end-to-end libpq ReadyForQuery accessor & hook test
my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf',
	"shared_preload_libraries = 'test_ready_for_query'\n");
$primary->start;

$primary->backup('my_backup');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'my_backup', has_streaming => 1);
$standby->start;

my ($out, $err) = run_command([
	'libpq_testclient', '--ready-for-query',
	$primary->connstr('postgres'),
	$standby->connstr('postgres'),
]);
is($err, '', 'no stderr from libpq_testclient --ready-for-query');
is($out, 'ready_for_query_ok', 'libpq ReadyForQuery accessors, hook, and replay/commit LSNs passed');

# 2. Malformed ReadyForQuery wire-frame boundary test via local mock TCP server
my $listen = IO::Socket::INET->new(
	LocalAddr => '127.0.0.1',
	LocalPort => 0,
	Proto     => 'tcp',
	Listen    => 1,
	ReuseAddr => 1,
);
if ($listen)
{
	my $port = $listen->sockport();
	my $pid = fork();
	die "fork failed: $!" unless defined $pid;
	if ($pid == 0)
	{
		my $cli = $listen->accept();
		if ($cli)
		{
			my $hdr;
			sysread($cli, $hdr, 8);
			my ($len, $code) = unpack('NN', $hdr);
			my $rem = $len - 8;
			my $discard;
			sysread($cli, $discard, $rem) if $rem > 0;
			# AuthenticationOk ('R', len=8, 0) + malformed 'Z' (klen=5 with only 1 byte left)
			my $auth_ok = 'R' . pack('NN', 8, 0);
			my $bad_rfq = 'Z' . pack('N', 7) . 'I' . chr(5) . 'T';
			syswrite($cli, $auth_ok . $bad_rfq);
			sleep 1;
			close($cli);
		}
		_exit(0);
	}
	close($listen);
	my ($m_out, $m_err) = run_command([
		'libpq_testclient', '--expect-bad-rfq',
		"host=127.0.0.1 port=$port dbname=postgres sslmode=disable gssencmode=disable",
	]);
	waitpid($pid, 0);
	is($m_err, '', 'malformed ReadyForQuery frame rejected cleanly without crash or OOB read');
}

$standby->stop;
$primary->stop;

done_testing();
