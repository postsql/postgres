# Technical Design & Implementation: UNION DISTINCT ON

## 1. Parser and Grammar Extensions

Grammar rules in `src/backend/parser/gram.y` were expanded to allow `DISTINCT ON` clauses following set operations:

```yacc
simple_select:
    ...
    | simple_select UNION opt_all select_distinct_on select_stmt
    ...
```

The parsed representation stores:
- `distinctClause`: List of expressions defining grouping keys.
- `distinctOnOrder`: Optional list of `SortBy` nodes defining the tie-breaker ordering.

---

## 2. Query Planner Refactoring (`prepunion.c` & `allpaths.c`)

In legacy PostgreSQL, set operation grouping lists (`SetOperationPath.groupClauses`) were tied directly to 1-based targetlist attribute positions (`1..n`).

### 2.1 Decoupled Grouping Lists
The planner was refactored so that grouping lists can represent a proper subset of the result columns:
- `generate_setop_grouplist()` now builds `SortGroupClause` entries matching the specific expressions present in `distinctClause`.
- Non-distinct columns are treated as arbitrary payload values.

### 2.2 Safety Checks (`UNSAFE_NOTIN_DISTINCTON_CLAUSE`)
When subqueries participate in `UNION DISTINCT ON`, pushing down filter predicates (`Quals`) on attributes that are not in the `DISTINCT ON` key list is strictly forbidden, as it would alter which representative row survives deduplication. The planner enforces this via `subquery_is_pushdown_safe()`.

---

## 3. Recursive Dynamic Pruning (`nodeRecursiveunion.c`)

![Recursive Pruning](../img/union_distinct_on_pruning.svg)

For recursive CTEs (`WITH RECURSIVE ... UNION DISTINCT ON`), execution is handled inside `ExecRecursiveUnion()` in `src/backend/executor/nodeRecursiveunion.c`:

### 3.1 State Structures
```c
typedef struct RecursiveUnionState {
    ...
    TupleHashTable distinct_hashtable; /* Tracks best tuple per key */
    Tuplestorestate *working_table;     /* Tuples queued for expansion */
    Tuplestorestate *intermediate_table;/* Newly discovered frontier */
    Tuplestorestate *output_tuplestore;  /* Final converged results */
    List *distinct_keys;                /* Key attribute projection */
    List *sort_keys;                    /* Tie-breaker sort expressions */
} RecursiveUnionState;
```

### 3.2 Evaluation Lifecycle per Recursive Tuple
For each tuple produced by the recursive step:
1. **Hash Lookup**: Lookup `distinct_keys` in `distinct_hashtable`.
2. **If Not Present**:
   - Insert tuple into `distinct_hashtable`.
   - Store in `intermediate_table` for downstream recursive exploration.
   - Buffer in `output_tuplestore`.
3. **If Present**:
   - Compare `sort_keys` of the candidate tuple against the stored tuple using `ApplySortComparator()`.
   - **Candidate is Worse or Equal**: Prune tuple immediately. Do not add to `intermediate_table`.
   - **Candidate is Strictly Better**:
     - Replace stored tuple in `distinct_hashtable`.
     - Queue candidate into `intermediate_table` so downstream paths are re-expanded with the superior cost!
     - Update buffered representation in `output_tuplestore`.
4. **Convergence**:
   When `intermediate_table` is empty, all states have stabilized, and `output_tuplestore` is returned to the outer consumer.
