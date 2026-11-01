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

# 3. 200-notification slow-client TCP burst coalescing test on "Commits visible at LSN 10 ms"
my $burst_listen = IO::Socket::INET->new(
	LocalAddr => '127.0.0.1',
	LocalPort => 0,
	Proto     => 'tcp',
	Listen    => 1,
	ReuseAddr => 1,
);
if ($burst_listen)
{
	my $bport = $burst_listen->sockport();
	my $bpid = fork();
	die "fork failed: $!" unless defined $bpid;
	if ($bpid == 0)
	{
		my $cli = $burst_listen->accept();
		if ($cli)
		{
			my $hdr;
			sysread($cli, $hdr, 8);
			my ($len, $code) = unpack('NN', $hdr);
			my $rem = $len - 8;
			my $discard;
			sysread($cli, $discard, $rem) if $rem > 0;

			my $stream = 'R' . pack('NN', 8, 0);
			for my $i (1 .. 200)
			{
				my $chan = "Commits visible at LSN 10 ms\0";
				my $extra = sprintf("2026-11-01T10:00:00.%06dZ 0/%08X\0", $i, $i);
				my $body = pack('N', 1234) . $chan . $extra;
				$stream .= 'A' . pack('N', 4 + length($body)) . $body;
				if ($i == 100)
				{
					my $ubody = pack('N', 1234) . "user_chan\0mid\0";
					$stream .= 'A' . pack('N', 4 + length($ubody)) . $ubody;
				}
			}
			$stream .= 'Z' . pack('N', 5) . 'I';
			syswrite($cli, $stream);
			sleep 1;
			close($cli);
		}
		_exit(0);
	}
	close($burst_listen);
	my ($b_out, $b_err) = run_command([
		'libpq_testclient', '--coalesce-burst',
		"host=127.0.0.1 port=$bport dbname=postgres sslmode=disable gssencmode=disable",
	]);
	waitpid($bpid, 0);
	is($b_err, '', 'no stderr from 200-notification burst test');
	is($b_out, 'coalesce_burst_ok', '200 buffered LSN notifications coalesced in place to latest payload');
}

$standby->stop;
$primary->stop;

done_testing();
