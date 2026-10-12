# How: User Guide for UNION DISTINCT ON

## 1. Syntax Overview

The syntax extends the SQL `UNION` operator:

```sql
SELECT ...
UNION DISTINCT ON ( key_column [, ...] [ORDER BY sort_column [ASC|DESC] [, ...]] )
SELECT ...
```

- **`key_column`**: The column(s) on which duplicate elimination is evaluated.
- **`ORDER BY sort_column`** (optional): Tie-breaking ordering expression. The first row evaluated according to this sort order is preserved; subsequent rows sharing the same key are eliminated.

---

## 2. Non-Recursive Set Operation Examples

### Example 1: Latest Customer Status Across Multiple Tables
Suppose you have a live operational table and an historical archive table, and you want the single most recent record per customer:

```sql
SELECT customer_id, status, updated_at
FROM current_customers
UNION DISTINCT ON (customer_id ORDER BY updated_at DESC)
SELECT customer_id, status, updated_at
FROM archived_customers;
```

### Example 2: Multi-Source Product Catalog Deduplication
Merge product catalogs from two suppliers, choosing the lowest price for each SKU:

```sql
SELECT sku, name, price, supplier_id
FROM supplier_a_catalog
UNION DISTINCT ON (sku ORDER BY price ASC)
SELECT sku, name, price, supplier_id
FROM supplier_b_catalog;
```

---

## 3. Recursive Dynamic Pruning (`WITH RECURSIVE`)

In recursive queries, `UNION DISTINCT ON` allows continuous state-space pruning, preventing infinite cycles and avoiding redundant path explorations.

### Example: Dijkstra's Shortest Path in Pure SQL

Given a graph table of road network edges:
```sql
CREATE TABLE edges (
    source_city TEXT,
    target_city TEXT,
    distance_km NUMERIC
);
```

Compute the shortest path and distance from `'New York'` to all reachable cities:

```sql
WITH RECURSIVE shortest_paths AS (
    -- Base case: seed from starting city
    SELECT
        'New York'::TEXT AS current_city,
        0::NUMERIC AS total_distance,
        ARRAY['New York'::TEXT] AS path

    UNION DISTINCT ON (current_city ORDER BY total_distance ASC)

    -- Recursive expansion: traverse edges
    SELECT
        e.target_city,
        sp.total_distance + e.distance_km,
        sp.path || e.target_city
    FROM shortest_paths sp
    JOIN edges e ON sp.current_city = e.source_city
    -- Cycle prevention: avoid revisiting cities in the same path
    WHERE NOT (e.target_city = ANY(sp.path))
)
SELECT current_city, total_distance, array_to_string(path, ' -> ') AS route
FROM shortest_paths
ORDER BY total_distance;
```

#### How the Engine Executes This:
1. When a shorter route to `target_city` is found in a subsequent iteration, the executor node detects that its `total_distance` is strictly less than the previously cached route.
2. The old suboptimal entry in the hash table is replaced with the new shorter route.
3. The improved tuple is re-queued into the recursive work table to propagate better distances to downstream vertices.
4. If a longer or equal-cost route is encountered, it is pruned immediately.

---

## 4. Inline `ORDER BY` in `SELECT DISTINCT ON`

You can also use inline `ORDER BY` syntax directly inside `SELECT DISTINCT ON`:

```sql
-- Standard PostgreSQL required:
-- SELECT DISTINCT ON (dept_id) dept_id, employee_name, salary FROM employees ORDER BY dept_id, salary DESC;

-- Now also allowed:
SELECT DISTINCT ON (dept_id ORDER BY salary DESC) dept_id, employee_name, salary
FROM employees;
```
When no external `ORDER BY` clause is present, the planner can utilize `HashAggregate` instead of requiring an expensive pre-sort.
