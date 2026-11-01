# Copyright (c) 2026, PostgreSQL Global Development Group

# Test coalescing "Commits visible at LSN ..." system NOTIFY channels
# on both primary (WAL flush LSN) and hot standby (WAL replay LSN).

use strict;
use warnings;

use IO::Select;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(sleep time);

$SIG{ALRM} = sub { die "Test timed out after 90 seconds\n"; };
alarm(90);

sub parse_lsn_hex
{
	my ($s) = @_;
	my ($hi, $lo) = split('/', $s);
	return (hex($hi) << 32) | hex($lo);
}

sub read_exact
{
	my ($sock, $len) = @_;
	my $buf = '';
	while (length($buf) < $len)
	{
		my $chunk;
		my $n = $sock->sysread($chunk, $len - length($buf));
		die "unexpected EOF on socket" if !defined($n) || $n == 0;
		$buf .= $chunk;
	}
	return $buf;
}

sub send_query
{
	my ($sock, $sql) = @_;
	my $payload = $sql . "\0";
	$sock->syswrite('Q' . pack('N', 4 + length($payload)) . $payload);
}

sub parse_notify_body
{
	my ($body) = @_;
	my $pid = unpack('N', substr($body, 0, 4));
	my ($chan, $extra) = split(/\0/, substr($body, 4), 3);
	return { pid => $pid, channel => $chan, extra => $extra };
}

sub read_until_rfq
{
	my ($sock) = @_;
	my @notifies = ();
	my @errors = ();
	while (1)
	{
		my $mtype = read_exact($sock, 1);
		my $mlen = unpack('N', read_exact($sock, 4));
		my $body = ($mlen > 4) ? read_exact($sock, $mlen - 4) : '';

		if ($mtype eq 'A')
		{
			push @notifies, parse_notify_body($body);
		}
		elsif ($mtype eq 'E')
		{
			push @errors, $body;
		}
		elsif ($mtype eq 'Z')
		{
			my $tx = substr($body, 0, 1);
			my %kv = ();
			my $off = 1;
			while ($off < length($body))
			{
				my $kl = ord(substr($body, $off++, 1));
				my $k = substr($body, $off, $kl);
				$off += $kl;
				my $vl = ord(substr($body, $off++, 1));
				my $v = $vl > 0 ? substr($body, $off, $vl) : '';
				$off += $vl;
				$kv{$k} = $v;
			}
			return ($tx, \%kv, \@notifies, \@errors);
		}
	}
}

# Drain any asynchronous NotificationResponse ('A') frames arriving within $timeout_s
sub drain_idle_notifies
{
	my ($sock, $timeout_s) = @_;
	my $sel = IO::Select->new($sock);
	my @notifies = ();
	my $deadline = time() + $timeout_s;

	while (1)
	{
		my $rem = $deadline - time();
		last if $rem <= 0;
		last unless $sel->can_read($rem);

		my $mtype = read_exact($sock, 1);
		my $mlen = unpack('N', read_exact($sock, 4));
		my $body = ($mlen > 4) ? read_exact($sock, $mlen - 4) : '';
		if ($mtype eq 'A')
		{
			push @notifies, parse_notify_body($body);
		}
	}
	return @notifies;
}

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->start;

if (!$primary->raw_connect_works())
{
	$primary->stop;
	plan skip_all => "this test requires working raw_connect()";
}

$primary->safe_psql('postgres',
	"CREATE ROLE regress_rfq_listener LOGIN; CREATE TABLE t_vis (id int);");

$primary->backup('bkp');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'bkp', has_streaming => 1);
$standby->start;

# 1. Negative & security checks: reserved channel prefix & regular LISTEN on standby
my ($ret, $out, $err) = $primary->psql('postgres',
	q{SET ROLE regress_rfq_listener; NOTIFY "Commits visible at LSN 10 ms", 'spoof';});
isnt($ret, 0, "user NOTIFY on system LSN channel rejected");
like($err, qr/reserved for system LSN notifications/, "ERRCODE_RESERVED_NAME reported for NOTIFY");

($ret, $out, $err) = $primary->psql('postgres',
	q{SELECT pg_notify('Commits visible at LSN custom', 'spoof');});
isnt($ret, 0, "pg_notify() on reserved prefix rejected");

($ret, $out, $err) = $standby->psql('postgres', q{LISTEN ordinary_channel;});
isnt($ret, 0, "ordinary LISTEN still prohibited on hot standby");
like($err, qr/cannot execute LISTEN during recovery/, "expected recovery error for ordinary LISTEN");

# 2. Connect raw sockets as unprivileged role to primary and standby in 'rich' mode
my $startup = pack('N', 196608)
  . "user\0regress_rfq_listener\0database\0postgres\0options\0-c ready_for_query_message=rich\0\0";
my $startup_pkt = pack('N', 4 + length($startup)) . $startup;

my $sock_p = $primary->raw_connect();
$sock_p->syswrite($startup_pkt);
read_until_rfq($sock_p);

my $sock_s = $standby->raw_connect();
$sock_s->syswrite($startup_pkt);
read_until_rfq($sock_s);

# 3. Subscribe on BOTH primary and standby: verify immediate (UTC timestamp, LSN) delivery
my $payload_re = qr/^(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{6}Z) ([0-9A-F]+\/[0-9A-F]{8})$/;

send_query($sock_p, q{LISTEN "Commits visible at LSN 50 ms";});
my ($tx_p1, $kv_p1, $not_p1) = read_until_rfq($sock_p);
is($kv_p1->{'L'}, '1', "primary ReadyForQuery L=1 after LISTEN");
is(scalar(@$not_p1), 1, "primary emits immediate initial notification upon LISTEN");
is($not_p1->[0]{channel}, "Commits visible at LSN 50 ms", "primary channel name matches");
like($not_p1->[0]{extra}, $payload_re, "primary payload is (UTC timestamp, LSN) pair");
my ($init_p_ts, $init_p_lsn_str) = ($not_p1->[0]{extra} =~ $payload_re);
my $init_p_lsn = parse_lsn_hex($init_p_lsn_str);

# Re-LISTEN on already-active channel should be a no-op (no duplicate initial notify)
send_query($sock_p, q{LISTEN "Commits visible at LSN 50 ms";});
my ($tx_p_dup, $kv_p_dup, $not_p_dup) = read_until_rfq($sock_p);
is(scalar(@$not_p_dup), 0, "duplicate LISTEN does not re-emit initial notification");

send_query($sock_s, q{LISTEN "Commits visible at LSN 50 ms";});
my ($tx_s1, $kv_s1, $not_s1) = read_until_rfq($sock_s);
is($kv_s1->{'L'}, '1', "standby ReadyForQuery L=1 after LISTEN in recovery");
is(scalar(@$not_s1), 1, "standby emits immediate initial notification upon LISTEN in recovery");
like($not_s1->[0]{extra}, $payload_re, "standby payload is (UTC timestamp, LSN) pair");
my ($init_s_ts, $init_s_lsn_str) = ($not_s1->[0]{extra} =~ $payload_re);
my $init_s_lsn = parse_lsn_hex($init_s_lsn_str);

# Verify pg_listening_channels() visibility on standby
send_query($sock_s, q{SELECT pg_listening_channels();});
my (undef, $kv_s_ch) = read_until_rfq($sock_s);
is($kv_s_ch->{'L'}, '1', "standby retains L=1 across read queries");

# 4. Burst of 25 commits on primary while both listeners are completely idle
my $burst_sql = join(" ", map { "INSERT INTO t_vis VALUES ($_);" } (1..25));
$primary->safe_psql('postgres', $burst_sql);
my $primary_flush_hex = $primary->safe_psql('postgres', "SELECT pg_current_wal_flush_lsn();");
my $target_lsn = parse_lsn_hex($primary_flush_hex);
$primary->wait_for_catchup($standby);

my @pushed_p = drain_idle_notifies($sock_p, 0.20);
my @pushed_s = drain_idle_notifies($sock_s, 0.20);

cmp_ok(scalar(@pushed_p), '>=', 1, "idle primary listener received pushed flush LSN notification");
cmp_ok(scalar(@pushed_p), '<=', 4, "primary 50 ms cadence coalesced 25-commit burst");
my (undef, $last_p_str) = ($pushed_p[-1]{extra} =~ $payload_re);
cmp_ok(parse_lsn_hex($last_p_str), '>=', $target_lsn, "primary pushed latest flush LSN");
cmp_ok(parse_lsn_hex($last_p_str), '>', $init_p_lsn, "primary LSN advanced monotonically");

cmp_ok(scalar(@pushed_s), '>=', 1, "idle standby listener received pushed replay LSN notification");
cmp_ok(scalar(@pushed_s), '<=', 4, "standby 50 ms cadence coalesced 25-commit burst");
my (undef, $last_s_str) = ($pushed_s[-1]{extra} =~ $payload_re);
cmp_ok(parse_lsn_hex($last_s_str), '>=', $target_lsn, "standby pushed latest replay LSN");
cmp_ok(parse_lsn_hex($last_s_str), '>', $init_s_lsn, "standby LSN advanced monotonically");

# 5. Quiet-cluster check: 0 notifications when no WAL advances
my @quiet_s = drain_idle_notifies($sock_s, 0.20);
is(scalar(@quiet_s), 0, "zero notifications emitted on quiet cluster");

# Verify pg_notify SLRU was untouched on standby
my $standby_notify_dir = $standby->data_dir . '/pg_notify';
opendir(my $dh, $standby_notify_dir) or die "opendir $standby_notify_dir: $!";
my @slru_files = grep { !/^\./ } readdir($dh);
closedir($dh);
cmp_ok(scalar(@slru_files), '<=', 1, "pg_notify SLRU has no page growth on standby");

# 6. Seamless standby promotion: existing subscription transitions to primary flush LSN
$standby->promote;
$standby->safe_psql('postgres', "INSERT INTO t_vis VALUES (999);");
my $promoted_flush_lsn = parse_lsn_hex(
	$standby->safe_psql('postgres', "SELECT pg_current_wal_flush_lsn();"));

my @post_promote = drain_idle_notifies($sock_s, 0.20);
cmp_ok(scalar(@post_promote), '>=', 1,
	"existing subscription continues pushing flush LSN after standby promotion");
my (undef, $prom_lsn_str) = ($post_promote[-1]{extra} =~ $payload_re);
cmp_ok(parse_lsn_hex($prom_lsn_str), '>=', $promoted_flush_lsn,
	"post-promotion notification reports promoted primary's flush LSN");

# 7. UNLISTEN resets L=0 and stops notifications
send_query($sock_p, q{UNLISTEN "Commits visible at LSN 50 ms";});
my ($tx_pu, $kv_pu) = read_until_rfq($sock_p);
is($kv_pu->{'L'}, '0', "L=0 after UNLISTEN on primary");

send_query($sock_s, q{UNLISTEN *;});
my ($tx_su, $kv_su) = read_until_rfq($sock_s);
is($kv_su->{'L'}, '0', "L=0 after UNLISTEN * on promoted standby");

$primary->safe_psql('postgres', "INSERT INTO t_vis VALUES (1000);");
my @after_unlisten = drain_idle_notifies($sock_p, 0.15);
is(scalar(@after_unlisten), 0, "no notifications delivered after UNLISTEN");

close($sock_p);
close($sock_s);
$standby->stop;
$primary->stop;

alarm(0);
done_testing();
