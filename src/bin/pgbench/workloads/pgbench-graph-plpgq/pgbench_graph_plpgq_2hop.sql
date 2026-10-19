--
-- pgbench-graph-plpgq: 2-Hop Friend-of-Friend Discovery & Scoring (LDBC IC-1 / IC-9 style)
-- Uses pl_pgq GRAPH_TABLE(snb_graph(), 'MATCH ... COLUMNS (...)') 2-hop path matching.
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
  FROM GRAPH_TABLE (snb_graph(),
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
         FROM GRAPH_TABLE (snb_graph(),
             'MATCH (src IS Person)-[IS knows]->(direct IS Person)
              COLUMNS (src.id AS src_id, direct.id AS direct_id)'
         ) d
        WHERE d.src_id = :person_id
          AND d.direct_id = g.fof_id
   )
 GROUP BY g.fof_id, g.first_name, g.last_name, g.city_id
 ORDER BY mutual_friends DESC, total_weight DESC, g.fof_id ASC
 LIMIT 10;
