# PostgreSQL Invisible / Excluded Indexes Extension (`invisible_indexes`)

## 1. Overview and Problem Statement

Database administrators and performance engineers frequently need to evaluate the impact of dropping an index without incurring the high I/O, CPU, and locking costs of dropping and potentially rebuilding large indexes (`DROP INDEX` / `CREATE INDEX CONCURRENTLY`).

An **invisible index** (also known as an **excluded index**) is maintained normally by the storage and execution engine during data modification (DML: `INSERT`, `UPDATE`, `DELETE`) and continues to enforce integrity constraints (such as `UNIQUE` and `PRIMARY KEY`), but is completely ignored by the query planner/optimizer when evaluating access paths for queries.

---

## 2. Architectural Comparison of Approaches

### Approach A: Native Core PostgreSQL Patch (`ALTER INDEX ... SET INVISIBLE`)
- **Syntax:**
  ```sql
  ALTER INDEX index_name [SET] INVISIBLE;
  ALTER INDEX index_name [SET] VISIBLE;
  ```
- **Engine Modifications Required:**
  1. **Parser (`gram.y`):** Add `INVISIBLE` and `VISIBLE` keywords to `AlterIndexStmt` grammar rules.
  2. **Catalogs (`pg_index.h`):** Add `indisinvisible bool` column to the `pg_index` catalog and update `BKI` catalog definitions and `CATALOG_VERSION_NO`.
  3. **DDL Handler (`indexcmds.c`):** Implement `AlterIndexVisibility(Oid indexRelid, bool invisible)` taking `ShareUpdateExclusiveLock` on the index relation and updating `pg_index.indisinvisible`.
  4. **Planner (`plancat.c`):** In `get_relation_info()`, check `indexRelation->rd_index->indisinvisible`. If true and `use_invisible_indexes` GUC is false, omit `IndexOptInfo` from `rel->indexlist`.
- **Pros:**
  - Standard SQL DDL syntax aligning with Oracle (`ALTER INDEX ... INVISIBLE`) and MySQL (`ALTER TABLE ... ALTER INDEX ... INVISIBLE`).
  - Zero extension dependency.
- **Cons:**
  - Requires branching and modifying core PostgreSQL server code.
  - Requires database cluster `initdb` or catalog migration. Incompatible with standard cloud PostgreSQL platforms (e.g. Cloud SQL, AlloyDB) without custom engine builds.

---

### Approach B: Index Reloptions (`ALTER INDEX ... SET (invisible = true)`)
- **Syntax:**
  ```sql
  ALTER INDEX index_name SET (invisible = true);
  ```
- **Investigation & Finding:**
  PostgreSQL provides `add_bool_reloption(kinds, name, ...)` in `access/reloptions.h`. However, core index access methods (such as `btree`, `hash`, `gin`, `gist`, `spgist`, `brin`) validate their options against hardcoded parse tables (e.g. `btopts` in `src/backend/access/nbt/nbtutils.c` and `default_reloptions()`).
  Attempting to set `(invisible = true)` on a built-in btree index produces:
  ```
  ERROR: reloption "invisible" not found in parse table
  ```
- **Conclusion:** Without patching core AM option tables in Postgres source, reloptions cannot be used for built-in btree/hash/gin/gist indexes from an external extension.

---

### Approach C: Extension with Planner Hooks and GUCs (Implemented)
- **Syntax & Controls:**
  - **Session & Transaction GUCs (supports duplicates, quotes, and schema qualifications):**
    ```sql
    SET excluded_indexes = 'i1, i2, i2';
    SET excluded_indexes = '"public"."idx1", "Other Schema"."My Index"';
    SET LOCAL excluded_indexes = 'idx1'; -- Transaction-scoped
    RESET excluded_indexes;
    ```
  - **Direct Syntactic DDL Analogues for `ALTER INDEX ... SET INVISIBLE`:**
    ```sql
    SELECT invisible_indexes.alter_index_invisible('idx1');
    SELECT invisible_indexes.alter_index_visible('idx1');
    -- Or with regclass:
    SELECT invisible_indexes.alter_index_invisible('public.idx1'::regclass);
    SELECT invisible_indexes.alter_index_visible('public.idx1'::regclass);
    ```
  - **Persistent Invisibility via DDL Comment (preserves existing user comments):**
    ```sql
    COMMENT ON INDEX idx1 IS 'invisible';
    COMMENT ON INDEX idx1 IS NULL;
    -- Or with functions:
    SELECT invisible_indexes.set_invisible('idx1', true);
    SELECT invisible_indexes.set_invisible('idx1', false);
    ```
  - **Visibility Introspection Functions & Views:**
    ```sql
    SELECT invisible_indexes.is_invisible('idx1');
    SELECT invisible_indexes.is_invisible('idx1'::regclass);
    SELECT * FROM invisible_indexes.invisible_indexes;
    ```
  - **Partitioned Table & Partition Index Support:**
    - Excluded parent partitioned indexes automatically propagate down to partition-local indexes across the entire inheritance hierarchy (`get_rel_relispartition` + `get_partition_parent`).
  - **Compatibility & Override GUCs:**
    ```sql
    SET invisible_indexes.use_invisible_indexes = on;  -- Planner considers all indexes
    SET invisible_indexes.enabled = off;               -- Completely disable planner hook
    ```
- **Engine Mechanics & Performance Optimizations:**
  - Uses `get_relation_info_hook` (PostgreSQL <= 18) and `build_simple_rel_hook` (PostgreSQL >= 19).
  - Employs dedicated `InvisibleIndexesContext` memory context to cache parsed GUC patterns across transactions, eliminating repeated per-query string splitting and memory allocation during query planning.
  - The storage and execution engines access `rel->rd_indexlist` directly in `ExecOpenIndices()`, which is completely untouched by the planner hook. Thus, write performance, index maintenance, and unique constraint enforcement remain 100% active and correct.

---

## 3. Security Model

In accordance with enterprise PostgreSQL security standards:
1. **Administrative Access:** `invisible_indexes.set_invisible()`, `alter_index_invisible()`, and `alter_index_visible()` enforce superuser authentication:
   ```c
   if (!superuser())
       ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE), ...));
   ```
2. **Revocation of Public Defaults:** Default execution privilege granted to `PUBLIC` is explicitly revoked in `invisible_indexes--1.0.sql` for all administrative routines.
3. **Locking Safety:** Metadata modifications acquire `ShareUpdateExclusiveLock` on index relations, preventing concurrent DDL conflicts without blocking concurrent `SELECT`, `INSERT`, `UPDATE`, or `DELETE`.
4. **Session Control Safety:** Session GUC `excluded_indexes` is marked `PGC_USERSET`, allowing users to safely test query plans in their own sessions without affecting other users or global database state.
