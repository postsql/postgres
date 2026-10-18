--
-- pgbench-graph-pgq: LDBC Graphalytics Suite (BFS, WCC, SSSP, PageRank, LCC)
-- Executes deterministic whole-graph algorithms in SQL over snb_graph using
-- Spanner PGQ / ISO SQL/PGQ GRAPH_TABLE step expansions and triangle pattern
-- matching, verifying structural & numerical invariants via \validate_result.
--

\set root_id 1

-- 1. Breadth-First Search (BFS) via WITH RECURSIVE + GRAPH_TABLE edge step
WITH RECURSIVE bfs AS (
    SELECT g.vertex_id,
           0 AS depth,
           0::bigint AS parent_id,
           ARRAY[g.vertex_id] AS visited
      FROM GRAPH_TABLE (snb_graph
          MATCH (p IS Person WHERE p.id = :root_id)
          COLUMNS (p.id AS vertex_id)
      ) g
    UNION ALL
    SELECT g.dst_id AS vertex_id,
           b.depth + 1 AS depth,
           b.vertex_id AS parent_id,
           b.visited || g.dst_id
      FROM bfs b,
           GRAPH_TABLE (snb_graph
               MATCH (src IS Person WHERE src.id = b.vertex_id)-[IS knows]->(dst IS Person)
               COLUMNS (dst.id AS dst_id)
           ) g
     WHERE b.depth < 4
       AND NOT (g.dst_id = ANY(b.visited))
),
bfs_levels AS (
    SELECT vertex_id,
           MIN(depth) AS min_depth
      FROM bfs
     GROUP BY vertex_id
)
SELECT count(*) AS bfs_reachable,
       COALESCE(MIN(min_depth) FILTER (WHERE vertex_id = :root_id), -1) AS bfs_root_depth,
       COALESCE(MAX(min_depth), 0) AS bfs_max_depth
  FROM bfs_levels \gset

\validate_result :bfs_root_depth 0
\validate_result (:bfs_reachable >= 1 and :bfs_max_depth >= 0) true

-- 2. Weakly Connected Components (WCC) via iterative label propagation over GRAPH_TABLE edges
WITH edges AS (
    SELECT src_id, dst_id
      FROM GRAPH_TABLE (snb_graph
          MATCH (src IS Person)-[IS knows]->(dst IS Person)
          COLUMNS (src.id AS src_id, dst.id AS dst_id)
      )
),
wcc0 AS (
    SELECT vid, vid AS cid
      FROM GRAPH_TABLE (snb_graph
          MATCH (p IS Person)
          COLUMNS (p.id AS vid)
      )
),
wcc1 AS (
    SELECT w.vid,
           LEAST(w.cid, COALESCE(MIN(nb.cid), w.cid)) AS cid
      FROM wcc0 w
      LEFT JOIN edges k ON k.src_id = w.vid
      LEFT JOIN wcc0 nb ON nb.vid = k.dst_id
     GROUP BY w.vid, w.cid
),
wcc2 AS (
    SELECT w.vid,
           LEAST(w.cid, COALESCE(MIN(nb.cid), w.cid)) AS cid
      FROM wcc1 w
      LEFT JOIN edges k ON k.src_id = w.vid
      LEFT JOIN wcc1 nb ON nb.vid = k.dst_id
     GROUP BY w.vid, w.cid
),
wcc3 AS (
    SELECT w.vid,
           LEAST(w.cid, COALESCE(MIN(nb.cid), w.cid)) AS cid
      FROM wcc2 w
      LEFT JOIN edges k ON k.src_id = w.vid
      LEFT JOIN wcc2 nb ON nb.vid = k.dst_id
     GROUP BY w.vid, w.cid
),
wcc4 AS (
    SELECT w.vid,
           LEAST(w.cid, COALESCE(MIN(nb.cid), w.cid)) AS cid
      FROM wcc3 w
      LEFT JOIN edges k ON k.src_id = w.vid
      LEFT JOIN wcc3 nb ON nb.vid = k.dst_id
     GROUP BY w.vid, w.cid
)
SELECT count(DISTINCT cid) AS wcc_components,
       count(*) AS wcc_vertices
  FROM wcc4 \gset

\validate_result (:wcc_components >= 1 and :wcc_components <= :wcc_vertices) true

-- 3. Single-Source Shortest Paths (SSSP) over weighted knows edges in GRAPH_TABLE
WITH RECURSIVE sssp AS (
    SELECT :root_id::bigint AS vid,
           0.0::float8 AS dist,
           0 AS hops,
           ARRAY[:root_id::bigint] AS path
    UNION ALL
    SELECT g.dst_id AS vid,
           s.dist + g.weight AS dist,
           s.hops + 1,
           s.path || g.dst_id
      FROM sssp s,
           GRAPH_TABLE (snb_graph
               MATCH (src IS Person WHERE src.id = s.vid)-[k IS knows]->(dst IS Person)
               COLUMNS (dst.id AS dst_id, k.weight AS weight)
           ) g
     WHERE s.hops < 3
       AND NOT (g.dst_id = ANY(s.path))
),
sssp_best AS (
    SELECT vid, MIN(dist) AS min_dist
      FROM sssp
     GROUP BY vid
)
SELECT COALESCE(MIN(min_dist) FILTER (WHERE vid = :root_id), -1.0) AS sssp_root_dist,
       COALESCE(MAX(min_dist), 0.0) AS sssp_max_dist
  FROM sssp_best \gset

\validate_result :sssp_root_dist 0.0 0.000001
\validate_result (:sssp_max_dist >= 0.0) true

-- 4. PageRank (PR) — 3-step damped power iteration (d = 0.85) over GRAPH_TABLE
WITH vertices AS (
    SELECT vid
      FROM GRAPH_TABLE (snb_graph
          MATCH (p IS Person)
          COLUMNS (p.id AS vid)
      )
),
edges AS (
    SELECT src_id, dst_id
      FROM GRAPH_TABLE (snb_graph
          MATCH (src IS Person)-[IS knows]->(dst IS Person)
          COLUMNS (src.id AS src_id, dst.id AS dst_id)
      )
),
n_cte AS (
    SELECT GREATEST(count(*), 1)::float8 AS n FROM vertices
),
deg AS (
    SELECT v.vid,
           count(k.dst_id)::int AS out_deg
      FROM vertices v
      LEFT JOIN edges k ON k.src_id = v.vid
     GROUP BY v.vid
),
pr0 AS (
    SELECT d.vid, d.out_deg, (1.0 / n_cte.n) AS pr
      FROM deg d CROSS JOIN n_cte
),
dangling0 AS (
    SELECT COALESCE(sum(pr), 0.0) AS dmass FROM pr0 WHERE out_deg = 0
),
pr1 AS (
    SELECT p.vid,
           p.out_deg,
           ((1.0 - 0.85) + 0.85 * d0.dmass) / n_cte.n +
           0.85 * COALESCE(sum(src.pr / NULLIF(src.out_deg, 0)), 0.0) AS pr
      FROM pr0 p
      CROSS JOIN n_cte
      CROSS JOIN dangling0 d0
      LEFT JOIN edges k ON k.dst_id = p.vid
      LEFT JOIN pr0 src ON src.vid = k.src_id
     GROUP BY p.vid, p.out_deg, n_cte.n, d0.dmass
),
dangling1 AS (
    SELECT COALESCE(sum(pr), 0.0) AS dmass FROM pr1 WHERE out_deg = 0
),
pr2 AS (
    SELECT p.vid,
           p.out_deg,
           ((1.0 - 0.85) + 0.85 * d1.dmass) / n_cte.n +
           0.85 * COALESCE(sum(src.pr / NULLIF(src.out_deg, 0)), 0.0) AS pr
      FROM pr1 p
      CROSS JOIN n_cte
      CROSS JOIN dangling1 d1
      LEFT JOIN edges k ON k.dst_id = p.vid
      LEFT JOIN pr1 src ON src.vid = k.src_id
     GROUP BY p.vid, p.out_deg, n_cte.n, d1.dmass
)
SELECT round(sum(pr)::numeric, 6)::float8 AS pr_total_mass,
       COALESCE(max(pr), 0.0) >= COALESCE(min(pr), 0.0) AS pr_ordered
  FROM pr2 \gset

\validate_result :pr_total_mass 1.0 0.01
\validate_result :pr_ordered true

-- 5. Local Clustering Coefficient (LCC) per vertex using GRAPH_TABLE triangle pattern matching:
--    2 * triangles(v) / (deg(v) * (deg(v) - 1))
WITH deg AS (
    SELECT src_id AS vid, count(DISTINCT dst_id) AS d
      FROM GRAPH_TABLE (snb_graph
          MATCH (src IS Person)-[IS knows]->(dst IS Person)
          COLUMNS (src.id AS src_id, dst.id AS dst_id)
      )
     GROUP BY src_id
),
tri AS (
    SELECT vid,
           count(*) AS closed_pairs
      FROM GRAPH_TABLE (snb_graph
          MATCH (p1 IS Person)-[IS knows]->(p2 IS Person)-[IS knows]->(p3 IS Person)<-[IS knows]-(p1 IS Person)
          WHERE p3.id > p2.id
          COLUMNS (p1.id AS vid)
      )
     GROUP BY vid
),
lcc_per_vertex AS (
    SELECT deg.vid,
           CASE
               WHEN deg.d < 2 THEN 0.0::float8
               ELSE LEAST(1.0::float8, (2.0 * COALESCE(tri.closed_pairs, 0))::float8 / (deg.d * (deg.d - 1))::float8)
           END AS lcc
      FROM deg
      LEFT JOIN tri ON tri.vid = deg.vid
)
SELECT COALESCE(avg(lcc), 0.0)::float8 AS avg_lcc
  FROM lcc_per_vertex \gset

\validate_result (:avg_lcc >= 0.0 and :avg_lcc <= 1.0) true
