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
-- pgbench-graph-pgq: 2-Hop Friend-of-Friend Discovery & Scoring (2-Hop Friend-of-Friend style)
-- Uses ISO SQL/PGQ GRAPH_TABLE 2-hop path matching over social_graph.
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
  FROM GRAPH_TABLE (social_graph
      MATCH (p1 IS Person WHERE p1.id = :person_id)-[k1 IS knows]->(p2 IS Person)-[k2 IS knows]->(p3 IS Person)
      WHERE p3.id <> p1.id
      COLUMNS (
          p3.id AS fof_id,
          p3.first_name AS first_name,
          p3.last_name AS last_name,
          p3.city_id AS city_id,
          k2.weight AS w2
      )
  ) g
 WHERE NOT EXISTS (
     SELECT 1
       FROM GRAPH_TABLE (social_graph
           MATCH (src IS Person WHERE src.id = :person_id)-[IS knows]->(direct IS Person WHERE direct.id = g.fof_id)
           COLUMNS (1 AS one)
       ) d
 )
 GROUP BY g.fof_id, g.first_name, g.last_name, g.city_id
 ORDER BY mutual_friends DESC, total_weight DESC, g.fof_id ASC
 LIMIT 10;
