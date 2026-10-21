/*
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * DISCLAIMER: This workload is an independent engineering benchmark designed
 * for database performance research and regression testing. While modeled
 * after graph traversal patterns published by the Graph Data Council (LDBC),
 * this is NOT an LDBC Benchmark, is NOT an implementation of an LDBC Benchmark,
 * and these results are NOT LDBC Benchmark Results.
 * LDBC, LDBC Benchmark, and LDBC Benchmark Results are registered trademarks
 * of the Graph Data Council. Distributed under CC-BY 4.0 attribution terms.
 */

--
-- pgbench-graph-sql-ud: Transactional Edge & Message Insertion with Reservoir Push & Watermark
--

\set src_id random_zipfian(1, 200 * :scale, 1.2)
\set dst_id random(1, 200 * :scale)

CALL pgbench_graph_add_edge(:src_id, :dst_id, 1.0, 'graph-stream-post', NULL) \gset edge_

\if reservoir_count('persons') >= 0
\reservoir_push persons :src_id
\reservoir_push persons :dst_id
\endif
\advance_watermark :edge_p_msg_id
