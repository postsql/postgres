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
-- pgbench-graph-sql-ud: 1-Hop Neighborhood & Recent Messages Query (1-Hop Neighborhood style)
--

\if reservoir_count('persons') >= 0
\reservoir_sample persons person_id random_zipfian(1, 200 * :scale, 1.2)
\else
\set person_id random_zipfian(1, 200 * :scale, 1.2)
\endif

SELECT p.id AS friend_id,
       p.first_name,
       p.last_name,
       k.weight,
       m.id AS msg_id,
       m.created_ts
  FROM pgbench_graph_knows k
  JOIN pgbench_graph_persons p ON p.id = k.dst_id
  LEFT JOIN LATERAL (
      SELECT id, created_ts
        FROM pgbench_graph_messages msg
       WHERE msg.creator_id = p.id
       ORDER BY msg.created_ts DESC
       LIMIT 2
  ) m ON true
 WHERE k.src_id = :person_id
 ORDER BY k.weight DESC, p.id ASC
 LIMIT 10;
