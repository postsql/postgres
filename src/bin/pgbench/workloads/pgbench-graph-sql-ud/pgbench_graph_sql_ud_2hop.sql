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
-- pgbench-graph-sql-ud: 2-Hop Friend-of-Friend Discovery & Scoring (2-Hop Friend-of-Friend style)
--

\if reservoir_count('persons') >= 0
\reservoir_sample persons person_id random_zipfian(1, 200 * :scale, 1.2)
\else
\set person_id random_zipfian(1, 200 * :scale, 1.2)
\endif

WITH hop1 AS (
    SELECT dst_id
      FROM pgbench_graph_knows
     WHERE src_id = :person_id
),
hop2 AS (
    SELECT k2.dst_id AS fof_id,
           count(*) AS mutual_friends,
           sum(k2.weight) AS total_weight
      FROM hop1 h1
      JOIN pgbench_graph_knows k2 ON k2.src_id = h1.dst_id
     WHERE k2.dst_id <> :person_id
       AND NOT EXISTS (
           SELECT 1
             FROM hop1 d
            WHERE d.dst_id = k2.dst_id
       )
     GROUP BY k2.dst_id
)
SELECT p.id,
       p.first_name,
       p.last_name,
       p.city_id,
       h2.mutual_friends,
       h2.total_weight
  FROM hop2 h2
  JOIN pgbench_graph_persons p ON p.id = h2.fof_id
 ORDER BY h2.mutual_friends DESC, h2.total_weight DESC, p.id ASC
 LIMIT 10;
