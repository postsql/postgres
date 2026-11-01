/*
 * libpq_testclient.c
 *		A test program for the libpq public API
 *
 * Copyright (c) 2022-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/interfaces/libpq/test/libpq_testclient.c
 */

#include "postgres_fe.h"

#ifndef WIN32
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "libpq-fe.h"

#ifndef LIBPQ_HAS_EXTENDED_READY_FOR_QUERY
#error "LIBPQ_HAS_EXTENDED_READY_FOR_QUERY not defined"
#endif

static void
print_ssl_library(void)
{
	const char *lib = PQsslAttribute(NULL, "library");

	if (!lib)
		fprintf(stderr, "SSL is not enabled\n");
	else
		printf("%s\n", lib);
}

typedef struct rfq_hook_ctx
{
	int			calls;
	PGTransactionStatusType last_tx_status;
	int			last_nfields;
} rfq_hook_ctx;

static void
test_rfq_hook(PGconn *conn, void *passthrough)
{
	rfq_hook_ctx *ctx = (rfq_hook_ctx *) passthrough;

	ctx->calls++;
	ctx->last_tx_status = PQtransactionStatus(conn);
	ctx->last_nfields = PQreadyForQueryFieldCount(conn);
}

static void
exec_expect_ok(PGconn *conn, const char *sql)
{
	PGresult   *res = PQexec(conn, sql);

	if (!res || (PQresultStatus(res) != PGRES_COMMAND_OK &&
				 PQresultStatus(res) != PGRES_TUPLES_OK))
	{
		fprintf(stderr, "query failed (%s): %s\n", sql, PQerrorMessage(conn));
		PQclear(res);
		PQfinish(conn);
		exit(1);
	}
	PQclear(res);
}

static int
run_ready_for_query_tests(const char *primary_conninfo,
						  const char *standby_conninfo)
{
	PGconn	   *pconn;
	PGconn	   *sconn;
	rfq_hook_ctx hook_ctx = {0};
	uint64_t	lsn1,
				lsn2,
				rlsn;
	int			vlen = -1;
	const char *ext_val;
	FILE	   *trace_fp;

	/* Null-safety checks */
	if (PQsessionHasTempTables(NULL) != -1 ||
		PQsessionHasHoldCursors(NULL) != -1 ||
		PQsessionHasPreparedStatements(NULL) != -1 ||
		PQsessionHasListenChannels(NULL) != -1 ||
		PQlastCommitLSN(NULL) != 0 ||
		PQlastReplayLSN(NULL) != 0 ||
		PQreadyForQueryFieldCount(NULL) != 0 ||
		PQreadyForQueryValue(NULL, "T", &vlen) != NULL ||
		vlen != 0)
	{
		fprintf(stderr, "NULL safety check failed\n");
		return 1;
	}

	pconn = PQconnectdb(primary_conninfo);
	if (PQstatus(pconn) != CONNECTION_OK)
	{
		fprintf(stderr, "primary connect failed: %s\n", PQerrorMessage(pconn));
		PQfinish(pconn);
		return 1;
	}

	/* Default 'plain' mode: session indicators return -1, field count is 0 */
	if (PQsessionHasTempTables(pconn) != -1 ||
		PQsessionHasHoldCursors(pconn) != -1 ||
		PQsessionHasPreparedStatements(pconn) != -1 ||
		PQsessionHasListenChannels(pconn) != -1 ||
		PQreadyForQueryFieldCount(pconn) != 0)
	{
		fprintf(stderr, "expected -1/0 in plain mode\n");
		PQfinish(pconn);
		return 1;
	}

	PQsetReadyForQueryHook(pconn, test_rfq_hook, &hook_ctx);
	trace_fp = tmpfile();
	if (trace_fp)
	{
		PQtrace(pconn, trace_fp);
		PQsetTraceFlags(pconn, PQTRACE_SUPPRESS_TIMESTAMPS);
	}

	exec_expect_ok(pconn, "SET ready_for_query_message = 'rich'");
	if (hook_ctx.calls != 1 ||
		hook_ctx.last_tx_status != PQTRANS_IDLE ||
		PQsessionHasTempTables(pconn) != 0 ||
		PQsessionHasHoldCursors(pconn) != 0 ||
		PQsessionHasPreparedStatements(pconn) != 0 ||
		PQsessionHasListenChannels(pconn) != 0 ||
		PQreadyForQueryFieldCount(pconn) < 5)
	{
		fprintf(stderr, "unexpected initial rich state\n");
		PQfinish(pconn);
		return 1;
	}

	/* Exercise T, H, P, L, custom hook key, and primary commit LSN 'l' */
	exec_expect_ok(pconn, "CREATE TEMP TABLE rfq_tmp(x int)");
	lsn1 = PQlastCommitLSN(pconn);
	ext_val = PQreadyForQueryValue(pconn, "temp_tables_info", &vlen);
	if (PQsessionHasTempTables(pconn) != 1 || lsn1 == 0 ||
		!ext_val || strcmp(ext_val, "has_temp_namespace") != 0 ||
		vlen != (int) strlen("has_temp_namespace"))
	{
		fprintf(stderr, "unexpected state after CREATE TEMP TABLE\n");
		PQfinish(pconn);
		return 1;
	}

	exec_expect_ok(pconn, "DECLARE rfq_cur CURSOR WITH HOLD FOR SELECT 1");
	if (PQsessionHasHoldCursors(pconn) != 1)
	{
		fprintf(stderr, "expected H=1\n");
		PQfinish(pconn);
		return 1;
	}

	exec_expect_ok(pconn, "PREPARE rfq_stmt AS SELECT 1");
	if (PQsessionHasPreparedStatements(pconn) != 1)
	{
		fprintf(stderr, "expected P=1\n");
		PQfinish(pconn);
		return 1;
	}

	exec_expect_ok(pconn, "LISTEN rfq_chan");
	lsn2 = PQlastCommitLSN(pconn);
	if (PQsessionHasListenChannels(pconn) != 1 || lsn2 < lsn1)
	{
		fprintf(stderr, "expected L=1 and monotonic commit LSN\n");
		PQfinish(pconn);
		return 1;
	}

	/* Enumerate fields via PQreadyForQueryField */
	for (int i = 0; i < PQreadyForQueryFieldCount(pconn); i++)
	{
		const char *k = NULL;
		const char *v = NULL;
		int			kl = -1,
					vl = -1;

		if (!PQreadyForQueryField(pconn, i, &k, &kl, &v, &vl) ||
			!k || kl <= 0 || !v || vl < 0)
		{
			fprintf(stderr, "PQreadyForQueryField(%d) failed\n", i);
			PQfinish(pconn);
			return 1;
		}
	}
	if (PQreadyForQueryField(pconn, -1, NULL, NULL, NULL, NULL) != 0 ||
		PQreadyForQueryField(pconn, 999, NULL, NULL, NULL, NULL) != 0)
	{
		fprintf(stderr, "PQreadyForQueryField out-of-range check failed\n");
		PQfinish(pconn);
		return 1;
	}

	/* DISCARD ALL resets T, H, P, L back to 0 */
	exec_expect_ok(pconn, "DISCARD ALL");
	/* DISCARD ALL also resets GUCs, so re-enable rich to inspect flags */
	exec_expect_ok(pconn, "SET ready_for_query_message = 'rich'");
	if (PQsessionHasTempTables(pconn) != 0 ||
		PQsessionHasHoldCursors(pconn) != 0 ||
		PQsessionHasPreparedStatements(pconn) != 0 ||
		PQsessionHasListenChannels(pconn) != 0)
	{
		fprintf(stderr, "expected all session indicators 0 after DISCARD ALL\n");
		PQfinish(pconn);
		return 1;
	}

	/* Switching back to 'plain' resets ternary indicators to -1 (unknown) */
	lsn2 = PQlastCommitLSN(pconn);
	exec_expect_ok(pconn, "SET ready_for_query_message = 'plain'");
	if (PQsessionHasTempTables(pconn) != -1 ||
		PQreadyForQueryFieldCount(pconn) != 0 ||
		PQlastCommitLSN(pconn) != lsn2)
	{
		fprintf(stderr, "unexpected state after switching back to plain\n");
		PQfinish(pconn);
		return 1;
	}

	/* PQreset clears server-reported LSNs while preserving the hook */
	PQreset(pconn);
	if (PQstatus(pconn) != CONNECTION_OK || PQlastCommitLSN(pconn) != 0)
	{
		fprintf(stderr, "PQreset did not clear server LSN state\n");
		PQfinish(pconn);
		return 1;
	}

	/* Generate a fresh commit on primary and verify standby 'r' replay LSN */
	exec_expect_ok(pconn, "SET ready_for_query_message = 'rich'");
	exec_expect_ok(pconn, "CREATE TABLE IF NOT EXISTS rfq_rep(x int); INSERT INTO rfq_rep VALUES (1)");
	lsn2 = PQlastCommitLSN(pconn);

	sconn = PQconnectdb(standby_conninfo);
	if (PQstatus(sconn) != CONNECTION_OK)
	{
		fprintf(stderr, "standby connect failed: %s\n", PQerrorMessage(sconn));
		PQfinish(sconn);
		PQfinish(pconn);
		return 1;
	}
	exec_expect_ok(sconn, "SET ready_for_query_message = 'rich'");
	for (int attempt = 0; attempt < 100; attempt++)
	{
		exec_expect_ok(sconn, "SELECT 1");
		rlsn = PQlastReplayLSN(sconn);
		if (rlsn >= lsn2)
			break;
		pg_usleep(20000);
	}
	if (PQlastReplayLSN(sconn) < lsn2 || PQlastCommitLSN(sconn) != 0)
	{
		fprintf(stderr, "standby replay LSN (%llu) did not reach primary commit LSN (%llu)\n",
				(unsigned long long) PQlastReplayLSN(sconn),
				(unsigned long long) lsn2);
		PQfinish(sconn);
		PQfinish(pconn);
		return 1;
	}

	/* PQparseCommitsVisibleLSN unit & boundary tests */
	{
		int64_t		ts = -1;
		uint64_t	plsn = 0;

		if (!PQparseCommitsVisibleLSN("1970-01-01T00:00:00.000000Z 0/00000001",
									  &ts, &plsn) ||
			ts != 0 || plsn != 1)
		{
			fprintf(stderr, "epoch PQparseCommitsVisibleLSN failed\n");
			return 1;
		}
		if (!PQparseCommitsVisibleLSN("2026-11-01T00:00:00.123456Z 1/0306EE20",
									  &ts, &plsn) ||
			ts != INT64CONST(1793491200123456) ||
			plsn != ((UINT64CONST(1) << 32) | UINT64CONST(0x0306EE20)))
		{
			fprintf(stderr, "2026 PQparseCommitsVisibleLSN failed (ts=%lld)\n",
					(long long) ts);
			return 1;
		}
		if (!PQparseCommitsVisibleLSN("2024-02-29T23:59:59.999999Z 0/1", &ts, &plsn) ||
			PQparseCommitsVisibleLSN("2025-02-29T23:59:59.999999Z 0/1", &ts, &plsn) ||
			PQparseCommitsVisibleLSN(NULL, &ts, &plsn) ||
			PQparseCommitsVisibleLSN("", &ts, &plsn) ||
			PQparseCommitsVisibleLSN("2026-11-01T00:00:00.123456 0/1", &ts, &plsn) ||
			PQparseCommitsVisibleLSN("2026-11-01T00:00:00.123456Z bad", &ts, &plsn))
		{
			fprintf(stderr, "boundary PQparseCommitsVisibleLSN check failed\n");
			return 1;
		}
	}

	/* Live standby LISTEN "Commits visible at LSN 10 ms" test */
	{
		PGnotify   *n;
		int64_t		ts = 0;
		uint64_t	plsn = 0;

		exec_expect_ok(sconn, "LISTEN \"Commits visible at LSN 10 ms\"");
		n = PQnotifies(sconn);
		if (!n ||
			strcmp(n->relname, "Commits visible at LSN 10 ms") != 0 ||
			!PQparseCommitsVisibleLSN(n->extra, &ts, &plsn) ||
			ts <= 0 || plsn < lsn2 ||
			PQlastVisibleLSNTimestamp(sconn) != ts ||
			PQnotifies(sconn) != NULL)
		{
			fprintf(stderr, "live standby Commits visible at LSN notify failed\n");
			if (n)
				PQfreemem(n);
			return 1;
		}
		PQfreemem(n);
		exec_expect_ok(sconn, "UNLISTEN *");
	}

	if (trace_fp)
	{
		PQuntrace(pconn);
		fclose(trace_fp);
	}
	PQfinish(sconn);
	PQfinish(pconn);
	printf("ready_for_query_ok\n");
	return 0;
}

int
main(int argc, char *argv[])
{
	if ((argc > 1) && !strcmp(argv[1], "--ssl"))
	{
		print_ssl_library();
		return 0;
	}
	if ((argc == 4) && !strcmp(argv[1], "--ready-for-query"))
		return run_ready_for_query_tests(argv[2], argv[3]);
	if ((argc == 3) && !strcmp(argv[1], "--expect-bad-rfq"))
	{
		PGconn	   *conn = PQconnectdb(argv[2]);
		int			ok = (PQstatus(conn) == CONNECTION_BAD &&
						  strstr(PQerrorMessage(conn), "malformed key-value data in \"Z\" message") != NULL);

		if (!ok)
			fprintf(stderr, "unexpected status=%d err=%s\n",
					(int) PQstatus(conn), PQerrorMessage(conn));
		PQfinish(conn);
		return ok ? 0 : 1;
	}
	if ((argc == 3) && !strcmp(argv[1], "--coalesce-burst"))
	{
		PGconn	   *conn = PQconnectdb(argv[2]);
		PGnotify   *n1,
				   *n2,
				   *n3;
		int64_t		ts = 0;
		uint64_t	lsn = 0;

		if (PQstatus(conn) != CONNECTION_OK)
		{
			fprintf(stderr, "burst connect failed: %s\n", PQerrorMessage(conn));
			PQfinish(conn);
			return 1;
		}
		n1 = PQnotifies(conn);
		n2 = PQnotifies(conn);
		n3 = PQnotifies(conn);
		if (!n1 || strcmp(n1->relname, "user_chan") != 0 ||
			strcmp(n1->extra, "mid") != 0 ||
			!n2 || strcmp(n2->relname, "Commits visible at LSN 10 ms") != 0 ||
			!PQparseCommitsVisibleLSN(n2->extra, &ts, &lsn) ||
			lsn != 200 || n3 != NULL)
		{
			fprintf(stderr, "burst coalescing failed: n1=%s n2=%s lsn=%llu n3=%p\n",
					n1 ? n1->relname : "NULL",
					n2 ? n2->extra : "NULL",
					(unsigned long long) lsn,
					(void *) n3);
			if (n1)
				PQfreemem(n1);
			if (n2)
				PQfreemem(n2);
			if (n3)
				PQfreemem(n3);
			PQfinish(conn);
			return 1;
		}
		PQfreemem(n1);
		PQfreemem(n2);
		PQfinish(conn);
		printf("coalesce_burst_ok\n");
		return 0;
	}

	printf("unsupported arguments\n");
	return 1;
}
