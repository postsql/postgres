--
-- pgbench-graph-plpgq: 1-Hop Neighborhood & Recent Messages Query (LDBC IS-3 / IC-2 style)
-- Uses pl_pgq GRAPH_TABLE(snb_graph(), 'MATCH ... COLUMNS (...)') pattern matching.
--

\if reservoir_count('persons') >= 0
\reservoir_sample persons person_id random_zipfian(1, 200 * :scale, 1.2)
\else
\set person_id random_zipfian(1, 200 * :scale, 1.2)
\endif

SELECT g.friend_id,
       g.first_name,
       g.last_name,
       g.weight,
       m.msg_id,
       m.created_ts
  FROM GRAPH_TABLE (snb_graph(),
      'MATCH (src IS Person)-[k IS knows]->(friend IS Person)
       COLUMNS (
           src.id AS src_id,
           friend.id AS friend_id,
           friend.first_name AS first_name,
           friend.last_name AS last_name,
           k.weight AS weight
       )'
  ) g
  LEFT JOIN LATERAL (
      SELECT gm.msg_id, gm.created_ts
        FROM GRAPH_TABLE (snb_graph(),
            'MATCH (msg IS Message)-[IS hasCreator]->(author IS Person)
             COLUMNS (
                 author.id AS author_id,
                 msg.id AS msg_id,
                 msg.created_ts AS created_ts
             )'
        ) gm
       WHERE gm.author_id = g.friend_id
       ORDER BY gm.created_ts DESC
       LIMIT 2
  ) m ON true
 WHERE g.src_id = :person_id
 ORDER BY g.weight DESC, g.friend_id ASC
 LIMIT 10;
