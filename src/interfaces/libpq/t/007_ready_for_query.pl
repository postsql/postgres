# Copyright (c) 2026, PostgreSQL Global Development Group

# Test libpq extended ReadyForQuery parsing, accessors, and malformed frame handling

use strict;
use warnings FATAL => 'all';

use IO::Select;
use IO::Socket::INET;
use IPC::Run ();
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

# Helper: run libpq_testclient against a single-process loopback mock server.
# Avoids fork() + Perl IO::Socket sharing issues on Windows.
sub run_mock_server_test
{
	my ($flag, $payload) = @_;

	my $listen = IO::Socket::INET->new(
		LocalAddr => '127.0.0.1',
		LocalPort => 0,
		Proto     => 'tcp',
		Listen    => 1,
	);
	ok(defined $listen, "listening socket opened for $flag")
	  or return (0, '', "socket failed: $!");

	my $port = $listen->sockport();
	my ($c_out, $c_err) = ('', '');
	my $h = IPC::Run::start(
		[
			'libpq_testclient', $flag,
			"host=127.0.0.1 port=$port dbname=postgres sslmode=disable gssencmode=disable",
		],
		'<' => \undef,
		'>' => \$c_out,
		'2>' => \$c_err,
		IPC::Run::timeout(30));

	my $sel = IO::Select->new($listen);
	my $cli = $sel->can_read(15) ? $listen->accept() : undef;
	close($listen);

	if ($cli)
	{
		binmode($cli);

		my $read_exact = sub {
			my ($sock, $want) = @_;
			my $buf = '';
			my $rsel = IO::Select->new($sock);
			while (length($buf) < $want)
			{
				last unless $rsel->can_read(10);
				my $n = sysread($sock, $buf, $want - length($buf), length($buf));
				last unless defined $n && $n > 0;
			}
			return $buf;
		};

		my $hdr = $read_exact->($cli, 8);
		if (length($hdr) == 8)
		{
			my ($len, $code) = unpack('NN', $hdr);
			my $rem = $len - 8;
			$read_exact->($cli, $rem) if $rem > 0;

			my $off = 0;
			while ($off < length($payload))
			{
				my $n = syswrite($cli, $payload, length($payload) - $off, $off);
				last unless defined $n && $n > 0;
				$off += $n;
			}
			shutdown($cli, 1);

			# Wait for client to close socket cleanly before close() to avoid RST
			my $csel = IO::Select->new($cli);
			while ($csel->can_read(5))
			{
				my $tmp = '';
				my $n = sysread($cli, $tmp, 1024);
				last if !defined($n) || $n <= 0;
			}
		}
		close($cli);
	}

	my $ok = $h->finish();
	$c_out =~ s/\r\n/\n/g;
	$c_err =~ s/\r\n/\n/g;
	chomp($c_out);
	return ($ok, $c_out, $c_err);
}

# 2. Malformed ReadyForQuery wire-frame boundary test via local mock TCP server
{
	# AuthenticationOk ('R', len=8, 0) + malformed 'Z' (klen=5 with only 1 byte left)
	my $auth_ok = 'R' . pack('NN', 8, 0);
	my $bad_rfq = 'Z' . pack('N', 7) . 'I' . chr(5) . 'T';
	my ($m_ok, $m_out, $m_err) =
	  run_mock_server_test('--expect-bad-rfq', $auth_ok . $bad_rfq);
	ok($m_ok, 'libpq_testclient --expect-bad-rfq exited 0');
	is($m_err, '', 'malformed ReadyForQuery frame rejected cleanly without crash or OOB read');
}

# 3. 200-notification slow-client TCP burst coalescing test on "Commits visible at LSN 10 ms"
{
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

	my ($b_ok, $b_out, $b_err) =
	  run_mock_server_test('--coalesce-burst', $stream);
	ok($b_ok, 'libpq_testclient --coalesce-burst exited 0');
	is($b_err, '', 'no stderr from 200-notification burst test');
	is($b_out, 'coalesce_burst_ok', '200 buffered LSN notifications coalesced in place to latest payload');
}

$standby->stop;
$primary->stop;

done_testing();
