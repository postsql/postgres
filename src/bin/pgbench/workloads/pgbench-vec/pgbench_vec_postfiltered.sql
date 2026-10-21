/*
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * DISCLAIMER: This workload is an independent engineering benchmark inspired
 * by the filtered vector search methodologies described in "VecBench: A
 * Controllable Benchmark for Filtered Vector Search" (Zhang et al., ACM SIGMOD).
 * It is not the official VecBench software suite and does not publish official
 * VecBench leaderboard ratings.
 */

--
-- pgbench-vec: Mode 3 Post-Filtered Vector Search with Oversampling
--
-- Oversamples candidates via vector similarity scan, then applies
-- scalar filtering and evaluates effective recall.
--

\set dim 8
\set k 5
\set k_oversample 60
\set cat random(0, 9)
\set qvec clustered_vector(:dim, 10, 0.35, 42, :cat)

-- Approximate / Index candidate oversampling + relational post-filter
WITH raw_candidates AS (
    SELECT id, category, price, is_active
      FROM pgbench_vec_items
     WHERE id <= 200 * :scale
     ORDER BY pgbench_vec_l2_sq(embedding, pgbench_vec_parse(:qvec)) ASC, id ASC
     LIMIT :k_oversample
)
SELECT id
  FROM raw_candidates
 WHERE category = :cat
 LIMIT :k
\gset_col retrieved

-- Exact ground-truth for post-filtered category
SELECT id
  FROM pgbench_vec_items
 WHERE category = :cat
 ORDER BY pgbench_vec_l2_sq(embedding, pgbench_vec_parse(:qvec)) ASC, id ASC
 LIMIT :k
\gset_col ground_truth

\record_recall name=r_postfilter retrieved ground_truth :k
