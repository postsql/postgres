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
-- pgbench-graph (plPGQ): Social Graph Interactive Cascading Deletes (ID1 - ID8 / DEL1 - DEL8)
-- Executes recursive reply-subthread cascade deletions using pl_pgq GRAPH_TABLE
-- step expansions, edge removals, and forum/member cleanup.
--

\set del_type random(1, 4)
\set event_ts (1700200000000000 + :client_id * 1000 + random(1, 50000))
\set p1_id random(1, 200 * :scale)
\set p2_id community_correlated_edge(:p1_id, 200 * :scale, 10, 0.75, 99)

\begin_dependent_write :event_ts CHANNEL 0

\if :del_type = 1
-- DEL6 / DEL7: Cascading Comment Sub-Thread Deletion via WITH RECURSIVE + GRAPH_TABLE
WITH RECURSIVE target_root AS (
    SELECT g.id AS id
      FROM GRAPH_TABLE (social_graph(),
          'MATCH (m IS Message WHERE m.parent_message_id IS NOT NULL AND m.id >= 100000)
           COLUMNS (m.id AS id)'
      ) g
     ORDER BY g.id DESC
     LIMIT 1
),
reply_tree AS (
    SELECT tr.id AS id
      FROM target_root tr
    UNION DISTINCT ON (id)
    SELECT g.child_id AS id
      FROM reply_tree rt
      JOIN GRAPH_TABLE (social_graph(),
          'MATCH (child IS Message)-[IS replyOf]->(parent IS Message)
           COLUMNS (child.id AS child_id, parent.id AS parent_id)'
      ) g ON g.parent_id = rt.id
),
del_likes AS (
    DELETE FROM pgbench_graph_likes l
     USING reply_tree rt
     WHERE l.message_id = rt.id
),
del_tags AS (
    DELETE FROM pgbench_graph_message_tags mt
     USING reply_tree rt
     WHERE mt.message_id = rt.id
)
DELETE FROM pgbench_graph_messages m
 USING reply_tree rt
 WHERE m.id = rt.id;

\elif :del_type = 2
-- DEL2 / DEL3: Remove a Post/Comment Like discovered via GRAPH_TABLE
DELETE FROM pgbench_graph_likes
 WHERE (person_id, message_id) IN (
     SELECT g.person_id, g.message_id
       FROM GRAPH_TABLE (social_graph(),
           'MATCH (p IS Person)-[IS likes]->(m IS Message)
            COLUMNS (p.id AS person_id, m.id AS message_id)'
       ) g
      WHERE g.person_id = :p1_id
      LIMIT 1
 );

\elif :del_type = 3
-- DEL5: Remove Forum Membership discovered via GRAPH_TABLE
DELETE FROM pgbench_graph_forum_members
 WHERE (forum_id, person_id) IN (
     SELECT g.forum_id, g.person_id
       FROM GRAPH_TABLE (social_graph(),
           'MATCH (f IS Forum)-[IS hasMember]->(p IS Person)
            COLUMNS (f.id AS forum_id, p.id AS person_id)'
       ) g
      WHERE g.person_id = :p1_id
        AND g.forum_id > 20 * :scale
      LIMIT 1
 );

\else
-- DEL8: Remove Bidirectional Friendship Edge (for dynamically added edges)
DELETE FROM pgbench_graph_knows
 WHERE (src_id = :p1_id AND dst_id = :p2_id AND created_ts >= 1700100000000000)
    OR (src_id = :p2_id AND dst_id = :p1_id AND created_ts >= 1700100000000000);
\endif

\end_dependent_write :event_ts CHANNEL 0
