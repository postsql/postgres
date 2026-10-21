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
-- pgbench-graph-plpgq: 2-Hop Friend-of-Friend Discovery & Scoring (2-Hop Friend-of-Friend style)
-- Uses pl_pgq GRAPH_TABLE(social_graph(), 'MATCH ... COLUMNS (...)') 2-hop path matching.
--

\if reservoir_count('persons') >= 0
\reservoir_sample persons person_id random_zipfian(1, 200 * :scale, 1.2)
\else
\set person_id random_zipfian(1, 200 * :scale, 1.2)
\endif

SELECT g.fof_id AS id,
       g.first_name,
       g.last_name,
       g.city_id,
       count(*) AS mutual_friends,
       sum(g.w2) AS total_weight
  FROM GRAPH_TABLE (social_graph(),
      'MATCH (p1 IS Person)-[k1 IS knows]->(p2 IS Person)-[k2 IS knows]->(p3 IS Person)
       WHERE p3.id <> p1.id
       COLUMNS (
           p1.id AS src_id,
           p3.id AS fof_id,
           p3.first_name AS first_name,
           p3.last_name AS last_name,
           p3.city_id AS city_id,
           k2.weight AS w2
       )'
  ) g
 WHERE g.src_id = :person_id
   AND NOT EXISTS (
       SELECT 1
         FROM GRAPH_TABLE (social_graph(),
             'MATCH (src IS Person)-[IS knows]->(direct IS Person)
              COLUMNS (src.id AS src_id, direct.id AS direct_id)'
         ) d
        WHERE d.src_id = :person_id
          AND d.direct_id = g.fof_id
   )
 GROUP BY g.fof_id, g.first_name, g.last_name, g.city_id
 ORDER BY mutual_friends DESC, total_weight DESC, g.fof_id ASC
 LIMIT 10;
