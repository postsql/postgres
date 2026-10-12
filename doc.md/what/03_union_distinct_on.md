# What: UNION DISTINCT ON Syntax and Recursive Pruning

## Overview

The `union_distinct_on` patch extends SQL set operations (`UNION`) with duplicate elimination based on an arbitrary subset of columns, mirroring the semantics of `SELECT DISTINCT ON`:

```sql
SELECT ... UNION DISTINCT ON (key_columns [ORDER BY sort_columns]) SELECT ...
```

This feature works across both non-recursive set operations and recursive common table expressions (`WITH RECURSIVE`):

1. **Non-Recursive Set Operations**:
   - Extends the SQL parser grammar (`gram.y`) to support `DISTINCT ON (target_list [ORDER BY sort_list])` directly following `UNION`.
   - Allows users to specify distinct keys and tie-breaker sorting expressions across combined result sets.
   - Refactors query planner set-operation grouping lists so that grouping clauses are decoupled from physical targetlist ordinal positions.
   - Enables both `HashAggregate` (when no sorting or matching sort order is required) and `Sort -> Unique` execution paths.
   - Enforces planner safety against invalid subquery qualification pushdowns via `UNSAFE_NOTIN_DISTINCTON_CLAUSE`.

2. **Recursive Set Operations (`WITH RECURSIVE`) with Dynamic Pruning**:
   - Enables `WITH RECURSIVE ... UNION DISTINCT ON (key_columns ORDER BY sort_columns) ...`.
   - Implements dynamic state-space pruning inside the `RecursiveUnion` executor node (`nodeRecursiveunion.c`).
   - During recursion:
     - Maintains a hashtable tracking the best tuple seen so far for each distinct key.
     - When a new tuple has a worse sort key than the recorded tuple, it is **pruned immediately** (not returned and not queued for further recursive expansion).
     - When a new tuple has a strictly better sort key, it **replaces** the existing entry in the hashtable and is queued back into the recursive work table to re-expand exploration from the improved state.
   - Emits optimal results buffered in a tuplestore upon convergence.

3. **Inline `ORDER BY` inside `SELECT DISTINCT ON`**:
   - Permits `SELECT DISTINCT ON (a ORDER BY b) ...` syntax in addition to standard `SELECT DISTINCT ON (a) ... ORDER BY a, b`.
   - Resolves planner ambiguities, enabling `HashAggregate` execution plans when the query ordering does not require physical sorting on the distinct prefix.
