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

#ifndef LIBPQ_HAS_WAIT_FOR_COMMIT_LSN
#error "LIBPQ_HAS_WAIT_FOR_COMMIT_LSN not defined"
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

typedef struct notice_counter
{
	int			count;
	char		last_msg[256];
} notice_counter;

static void
count_notices(void *arg, const char *message)
{
	notice_counter *nc = (notice_counter *) arg;

	nc->count++;
	strlcpy(nc->last_msg, message ? message : "", sizeof(nc->last_msg));
}

static int
query_single_int(PGconn *conn, const char *sql)
{
	PGresult   *res = PQexec(conn, sql);
	int			val;

	if (!res || PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) != 1)
	{
		fprintf(stderr, "query_single_int failed (%s): %s\n",
				sql, PQerrorMessage(conn));
		PQclear(res);
		exit(1);
	}
	val = atoi(PQgetvalue(res, 0, 0));
	PQclear(res);
	return val;
}

static void
drain_async_conn(PGconn *conn)
{
	PGresult   *res;

	while ((res = PQgetResult(conn)) != NULL)
	{
		if (PQresultStatus(res) != PGRES_TUPLES_OK &&
			PQresultStatus(res) != PGRES_COMMAND_OK)
		{
			fprintf(stderr, "async helper failed: %s\n", PQerrorMessage(conn));
			PQclear(res);
			exit(1);
		}
		PQclear(res);
	}
}

static int
run_wait_for_commit_lsn_tests(const char *primary_conninfo,
							  const char *standby1_conninfo,
							  const char *standby2_conninfo)
{
	PGconn	   *pconn;
	PGconn	   *ro1;
	PGconn	   *ro2;
	PGconn	   *ctrl1;
	PGconn	   *ctrl2;
	PGconn	   *ro_list[2];
	notice_counter nc = {0};
	PQwaitCommitLSNOpts opts;
	uint64_t	lsn;

	pconn = PQconnectdb(primary_conninfo);
	ro1 = PQconnectdb(standby1_conninfo);
	ro2 = PQconnectdb(standby2_conninfo);
	ctrl1 = PQconnectdb(standby1_conninfo);
	ctrl2 = PQconnectdb(standby2_conninfo);

	if (PQstatus(pconn) != CONNECTION_OK ||
		PQstatus(ro1) != CONNECTION_OK ||
		PQstatus(ro2) != CONNECTION_OK ||
		PQstatus(ctrl1) != CONNECTION_OK ||
		PQstatus(ctrl2) != CONNECTION_OK)
	{
		fprintf(stderr, "connection setup failed\n");
		return 1;
	}

	PQsetNoticeProcessor(ro1, count_notices, &nc);

	/* 1. Boundary & negative input checks */
	if (PQassociateReadOnlyConn(NULL, ro1) != 0 ||
		PQassociateReadOnlyConn(pconn, NULL) != 0 ||
		PQassociateReadOnlyConn(pconn, pconn) != 0 ||
		PQassociateReadOnlyConns(pconn, NULL, 1) != 0 ||
		PQassociateReadOnlyConns(pconn, NULL, -1) != 0 ||
		PQsetWaitForCommitLSN(NULL, 1) != 0 ||
		PQgetWaitForCommitLSN(NULL) != 0 ||
		PQgetSatisfiedCommitLSN(NULL) != 0 ||
		PQgetAssociatedPrimaryConn(NULL) != NULL)
	{
		fprintf(stderr, "boundary checks failed\n");
		return 1;
	}

	memset(&opts, 0, sizeof(opts));
	opts.timeout_ms = -5;
	if (PQsetWaitForCommitLSNOpts(ro1, 1, &opts) != 0)
	{
		fprintf(stderr, "negative timeout_ms should be rejected\n");
		return 1;
	}
	opts.timeout_ms = 0;
	opts.mode = (PQWaitLSNMode) 99;
	if (PQsetWaitForCommitLSNOpts(ro1, 1, &opts) != 0)
	{
		fprintf(stderr, "invalid mode should be rejected\n");
		return 1;
	}

	/* 2. Associate two open RO connections with primary */
	ro_list[0] = ro1;
	ro_list[1] = ro2;
	if (!PQassociateReadOnlyConns(pconn, ro_list, 2) ||
		PQgetAssociatedPrimaryConn(ro1) != pconn ||
		PQgetAssociatedPrimaryConn(ro2) != pconn ||
		PQsessionHasTempTables(pconn) != 0)
	{
		fprintf(stderr, "PQassociateReadOnlyConns failed\n");
		return 1;
	}

	/* Role-mismatch guards */
	if (PQassociateReadOnlyConn(ro1, ro2) != 0 ||
		PQassociateReadOnlyConn(pconn, pconn) != 0 ||
		PQsetWaitForCommitLSN(pconn, 1) != 0 ||
		PQdissociateReadOnlyConn(pconn) != 0 ||
		PQsetMinTargetCommitLSN(pconn, 100) != 0)
	{
		fprintf(stderr, "role-mismatch guards failed\n");
		return 1;
	}
	/* Clear expected error messages on pconn/ro1 */
	exec_expect_ok(pconn, "SELECT 1");

	/* Enable WAIT on ro1, leave ro2 disabled */
	if (!PQsetWaitForCommitLSN(ro1, 1) ||
		PQgetWaitForCommitLSN(ro1) != 1 ||
		PQgetWaitForCommitLSN(ro2) != 0)
	{
		fprintf(stderr, "PQsetWaitForCommitLSN state mismatch\n");
		return 1;
	}

	/* 3a. Simple Query causal read-your-writes with delayed standby replay */
	exec_expect_ok(ctrl1, "SELECT pg_wal_replay_pause()");
	exec_expect_ok(ctrl2, "SELECT pg_wal_replay_pause()");
	exec_expect_ok(pconn, "INSERT INTO wait_lsn_tbl VALUES (101)");
	lsn = PQlastCommitLSN(pconn);

	/* ro2 has WAIT disabled -> immediately sees 0 while paused */
	if (query_single_int(ro2, "SELECT count(*) FROM wait_lsn_tbl WHERE id = 101") != 0)
	{
		fprintf(stderr, "expected ro2 (WAIT disabled) to see 0 while paused\n");
		return 1;
	}
	exec_expect_ok(ctrl2, "SELECT pg_wal_replay_resume()");

	/* Schedule resume on standby1 in 100ms; ro1 must wait and see 1 */
	if (!PQsendQuery(ctrl1, "SELECT pg_sleep(0.10), pg_wal_replay_resume()"))
		return 1;
	if (query_single_int(ro1, "SELECT count(*) FROM wait_lsn_tbl WHERE id = 101") != 1 ||
		PQgetSatisfiedCommitLSN(ro1) < lsn)
	{
		fprintf(stderr, "ro1 Simple Query did not wait for commit LSN\n");
		return 1;
	}
	drain_async_conn(ctrl1);

	/* 3b. Extended Query (PQexecParams) 0-RTT coalescing */
	exec_expect_ok(ctrl1, "SELECT pg_wal_replay_pause()");
	exec_expect_ok(pconn, "INSERT INTO wait_lsn_tbl VALUES (102)");
	lsn = PQlastCommitLSN(pconn);
	if (!PQsendQuery(ctrl1, "SELECT pg_sleep(0.10), pg_wal_replay_resume()"))
		return 1;
	{
		const char *paramValues[1] = {"102"};
		PGresult   *res = PQexecParams(ro1,
									   "SELECT count(*) FROM wait_lsn_tbl WHERE id = $1::int",
									   1, NULL, paramValues, NULL, NULL, 0);

		if (!res || PQresultStatus(res) != PGRES_TUPLES_OK ||
			atoi(PQgetvalue(res, 0, 0)) != 1 ||
			PQgetSatisfiedCommitLSN(ro1) < lsn ||
			PQsessionHasPreparedStatements(ro1) != 0)
		{
			fprintf(stderr, "ro1 PQexecParams wait failed\n");
			PQclear(res);
			return 1;
		}
		PQclear(res);
	}
	drain_async_conn(ctrl1);

	/* 3c. Unnamed prepared statement ("") preserved across internal WAIT */
	{
		const char *paramValues[1] = {"103"};
		PGresult   *res;
		PGresult   *pres = PQprepare(ro1, "",
									 "SELECT count(*) FROM wait_lsn_tbl WHERE id = $1::int",
									 0, NULL);

		if (!pres || PQresultStatus(pres) != PGRES_COMMAND_OK)
		{
			fprintf(stderr, "PQprepare unnamed failed: %s\n", PQerrorMessage(ro1));
			PQclear(pres);
			return 1;
		}
		PQclear(pres);

		exec_expect_ok(ctrl1, "SELECT pg_wal_replay_pause()");
		exec_expect_ok(pconn, "INSERT INTO wait_lsn_tbl VALUES (103)");
		lsn = PQlastCommitLSN(pconn);
		if (!PQsendQuery(ctrl1, "SELECT pg_sleep(0.10), pg_wal_replay_resume()"))
			return 1;

		res = PQexecPrepared(ro1, "", 1, paramValues, NULL, NULL, 0);

		if (!res || PQresultStatus(res) != PGRES_TUPLES_OK ||
			atoi(PQgetvalue(res, 0, 0)) != 1 ||
			PQgetSatisfiedCommitLSN(ro1) < lsn ||
			PQsessionHasPreparedStatements(ro1) != 0)
		{
			fprintf(stderr, "ro1 PQexecPrepared(\"\") wait failed (P=%d)\n",
					PQsessionHasPreparedStatements(ro1));
			PQclear(res);
			return 1;
		}
		PQclear(res);
		drain_async_conn(ctrl1);
	}

	/* 3d. Pipeline mode coalescing */
	exec_expect_ok(ctrl1, "SELECT pg_wal_replay_pause()");
	exec_expect_ok(pconn, "INSERT INTO wait_lsn_tbl VALUES (104)");
	lsn = PQlastCommitLSN(pconn);
	if (!PQsendQuery(ctrl1, "SELECT pg_sleep(0.10), pg_wal_replay_resume()"))
		return 1;
	{
		const char *p1[1] = {"104"};
		const char *p2[1] = {"101"};
		PGresult   *r1,
				   *r2,
				   *sync_res;

		if (!PQenterPipelineMode(ro1) ||
			!PQsendQueryParams(ro1, "SELECT count(*) FROM wait_lsn_tbl WHERE id = $1::int",
							   1, NULL, p1, NULL, NULL, 0) ||
			!PQsendQueryParams(ro1, "SELECT count(*) FROM wait_lsn_tbl WHERE id = $1::int",
							   1, NULL, p2, NULL, NULL, 0) ||
			!PQpipelineSync(ro1))
		{
			fprintf(stderr, "pipeline dispatch failed: %s\n", PQerrorMessage(ro1));
			return 1;
		}

		r1 = PQgetResult(ro1);
		if (!r1 || PQresultStatus(r1) != PGRES_TUPLES_OK ||
			atoi(PQgetvalue(r1, 0, 0)) != 1 || PQgetResult(ro1) != NULL)
		{
			fprintf(stderr, "pipeline r1 failed\n");
			return 1;
		}
		PQclear(r1);

		r2 = PQgetResult(ro1);
		if (!r2 || PQresultStatus(r2) != PGRES_TUPLES_OK ||
			atoi(PQgetvalue(r2, 0, 0)) != 1 || PQgetResult(ro1) != NULL)
		{
			fprintf(stderr, "pipeline r2 failed\n");
			return 1;
		}
		PQclear(r2);

		sync_res = PQgetResult(ro1);
		if (!sync_res || PQresultStatus(sync_res) != PGRES_PIPELINE_SYNC ||
			!PQexitPipelineMode(ro1))
		{
			fprintf(stderr, "pipeline sync/exit failed\n");
			return 1;
		}
		PQclear(sync_res);
	}
	drain_async_conn(ctrl1);

	/* 4. Unset WAIT on ro1 and enable on ro2 */
	if (!PQsetWaitForCommitLSN(ro1, 0) || !PQsetWaitForCommitLSN(ro2, 1))
		return 1;
	exec_expect_ok(ctrl1, "SELECT pg_wal_replay_pause()");
	exec_expect_ok(ctrl2, "SELECT pg_wal_replay_pause()");
	exec_expect_ok(pconn, "INSERT INTO wait_lsn_tbl VALUES (105)");
	if (query_single_int(ro1, "SELECT count(*) FROM wait_lsn_tbl WHERE id = 105") != 0)
	{
		fprintf(stderr, "expected ro1 (now unset) to return 0 immediately\n");
		return 1;
	}
	exec_expect_ok(ctrl1, "SELECT pg_wal_replay_resume()");
	if (!PQsendQuery(ctrl2, "SELECT pg_sleep(0.10), pg_wal_replay_resume()"))
		return 1;
	if (query_single_int(ro2, "SELECT count(*) FROM wait_lsn_tbl WHERE id = 105") != 1)
	{
		fprintf(stderr, "expected ro2 (now enabled) to wait and return 1\n");
		return 1;
	}
	drain_async_conn(ctrl2);

	/* 5. Timeout error isolation (no_throw=0) vs 0-RTT coalescing (no_throw=1) */
	exec_expect_ok(ctrl1, "SELECT pg_wal_replay_pause()");
	exec_expect_ok(pconn, "INSERT INTO wait_lsn_tbl VALUES (106)");
	lsn = PQlastCommitLSN(pconn);

	memset(&opts, 0, sizeof(opts));
	opts.mode = PQWAIT_LSN_STANDBY_REPLAY;
	opts.timeout_ms = 50;
	opts.no_throw = 0;
	if (!PQsetWaitForCommitLSNOpts(ro1, 1, &opts))
		return 1;

	/* Simple Query with no_throw=0: "SELECT 1/0" must NEVER reach standby1! */
	{
		PGresult   *res = PQexec(ro1, "SELECT 1 / 0");

		if (!res || PQresultStatus(res) != PGRES_FATAL_ERROR ||
			strstr(PQerrorMessage(ro1), "timed out while waiting for target LSN") == NULL ||
			strstr(PQerrorMessage(ro1), "division by zero") != NULL)
		{
			fprintf(stderr, "Simple Query no_throw=0 error isolation failed: %s\n",
					PQerrorMessage(ro1));
			PQclear(res);
			return 1;
		}
		PQclear(res);
	}

	/* Extended Query with no_throw=0: ignore_till_sync skips "SELECT 1/0"! */
	{
		PGresult   *res = PQexecParams(ro1, "SELECT 1 / 0",
									   0, NULL, NULL, NULL, NULL, 0);

		if (!res || PQresultStatus(res) != PGRES_FATAL_ERROR ||
			strstr(PQerrorMessage(ro1), "timed out while waiting for target LSN") == NULL ||
			strstr(PQerrorMessage(ro1), "division by zero") != NULL)
		{
			fprintf(stderr, "Extended Query no_throw=0 error isolation failed: %s\n",
					PQerrorMessage(ro1));
			PQclear(res);
			return 1;
		}
		PQclear(res);
	}

	/* Simple Query with no_throw=1: coalesces in 0 extra RTT + emits warning */
	opts.no_throw = 1;
	if (!PQsetWaitForCommitLSNOpts(ro1, 1, &opts))
		return 1;
	nc.count = 0;
	if (query_single_int(ro1, "SELECT count(*) FROM wait_lsn_tbl WHERE id = 106") != 0 ||
		nc.count != 1 ||
		strstr(nc.last_msg, "timeout") == NULL ||
		PQgetSatisfiedCommitLSN(ro1) >= lsn)
	{
		fprintf(stderr, "Simple Query no_throw=1 behavior mismatch (notices=%d msg=%s)\n",
				nc.count, nc.last_msg);
		return 1;
	}

	/* Resume standby1 and verify ro1 now catches up and updates satisfied_lsn */
	exec_expect_ok(ctrl1, "SELECT pg_wal_replay_resume()");
	opts.timeout_ms = 5000;
	opts.no_throw = 0;
	PQsetWaitForCommitLSNOpts(ro1, 1, &opts);
	if (query_single_int(ro1, "SELECT count(*) FROM wait_lsn_tbl WHERE id = 106") != 1 ||
		PQgetSatisfiedCommitLSN(ro1) < lsn)
	{
		fprintf(stderr, "ro1 failed to catch up after resume\n");
		return 1;
	}

	/* 6. In-transaction policies (SKIP_WARN vs ERROR) */
	exec_expect_ok(ctrl1, "SELECT pg_wal_replay_pause()");
	exec_expect_ok(ro1, "BEGIN");
	exec_expect_ok(pconn, "INSERT INTO wait_lsn_tbl VALUES (107)");
	nc.count = 0;
	if (query_single_int(ro1, "SELECT 1") != 1 || nc.count != 1 ||
		strstr(nc.last_msg, "skipping WAIT FOR LSN") == NULL)
	{
		fprintf(stderr, "PQWAIT_INTRANS_SKIP_WARN failed\n");
		return 1;
	}
	opts.intrans_policy = PQWAIT_INTRANS_ERROR;
	PQsetWaitForCommitLSNOpts(ro1, 1, &opts);
	if (PQexec(ro1, "SELECT 1") != NULL ||
		strstr(PQerrorMessage(ro1), "open transaction block") == NULL)
	{
		fprintf(stderr, "PQWAIT_INTRANS_ERROR failed\n");
		return 1;
	}
	PQsetWaitForCommitLSN(ro1, 0);
	exec_expect_ok(ro1, "ROLLBACK");
	exec_expect_ok(ctrl1, "SELECT pg_wal_replay_resume()");
	PQsetWaitForCommitLSNOpts(ro1, 1, NULL);

	/* 7. Zero-wait fast path via LISTEN "Commits visible at LSN 10 ms" */
	exec_expect_ok(ro1, "LISTEN \"Commits visible at LSN 10 ms\"");
	{
		PGnotify   *n;

		while ((n = PQnotifies(ro1)) != NULL)
			PQfreemem(n);
	}
	exec_expect_ok(pconn, "INSERT INTO wait_lsn_tbl VALUES (108)");
	lsn = PQlastCommitLSN(pconn);
	for (int attempt = 0; attempt < 200; attempt++)
	{
		PGnotify   *n;

		if (!PQconsumeInput(ro1))
			return 1;
		while ((n = PQnotifies(ro1)) != NULL)
			PQfreemem(n);
		if (PQgetSatisfiedCommitLSN(ro1) >= lsn)
			break;
		pg_usleep(10000);
	}
	if (PQgetSatisfiedCommitLSN(ro1) < lsn)
	{
		fprintf(stderr, "NOTIFY did not advance PQgetSatisfiedCommitLSN\n");
		return 1;
	}
	/* Now satisfied_lsn >= lsn BEFORE query -> executes on 0-wait fast path! */
	exec_expect_ok(ctrl1, "SELECT /* fastpath_begin_108 */ 1");
	if (query_single_int(ro1, "SELECT /* fastpath_no_wait_108 */ count(*) FROM wait_lsn_tbl WHERE id = 108") != 1)
		return 1;
	exec_expect_ok(ro1, "UNLISTEN *");

	/* 8. Unprivileged role check + out-of-order PQfinish(pconn) */
	exec_expect_ok(pconn, "INSERT INTO wait_lsn_tbl VALUES (109)");
	lsn = PQlastCommitLSN(pconn);

	/* Close primary FIRST while ro1 & ro2 remain open */
	PQfinish(pconn);
	pconn = NULL;

	if (PQgetAssociatedPrimaryConn(ro1) != NULL ||
		PQgetAssociatedPrimaryConn(ro2) != NULL)
	{
		fprintf(stderr, "expected NULL associated primary after PQfinish(pconn)\n");
		return 1;
	}

	/* ro1 still honours the last published commit_lsn under unprivileged role */
	exec_expect_ok(ro1, "SET ROLE regress_wait_lsn_unpriv");
	if (query_single_int(ro1, "SELECT count(*) FROM wait_lsn_tbl WHERE id = 109") != 1 ||
		PQgetSatisfiedCommitLSN(ro1) < lsn)
	{
		fprintf(stderr, "ro1 query after PQfinish(pconn) under unprivileged role failed\n");
		return 1;
	}
	exec_expect_ok(ro1, "RESET ROLE");

	/* Explicit PQdissociateReadOnlyConn + standalone PQsetMinTargetCommitLSN */
	if (!PQdissociateReadOnlyConn(ro2) ||
		!PQsetMinTargetCommitLSN(ro2, lsn) ||
		!PQsetWaitForCommitLSN(ro2, 1) ||
		query_single_int(ro2, "SELECT count(*) FROM wait_lsn_tbl WHERE id = 109") != 1)
	{
		fprintf(stderr, "standalone PQsetMinTargetCommitLSN failed\n");
		return 1;
	}

	PQfinish(ctrl1);
	PQfinish(ctrl2);
	PQfinish(ro1);
	PQfinish(ro2);
	printf("wait_for_commit_lsn_ok\n");
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
	if ((argc == 5) && !strcmp(argv[1], "--wait-for-commit-lsn"))
		return run_wait_for_commit_lsn_tests(argv[2], argv[3], argv[4]);
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
