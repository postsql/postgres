# Why: Rationale for pgbench Refactoring and SQL Extension

## Motivation and Industry Context

`pgbench` is PostgreSQL's official benchmarking and load-testing tool. Over twenty years of active development, `pgbench.c` expanded into an unwieldy 6,000-line monolithic file combining command-line parsing, socket event loops, expression parsing, variable evaluation, client state machines, transaction statistics, and initialization scripts.

This monolithic architecture caused severe friction:
1. **High Barrier to Extension**:
   Adding new benchmark commands, custom protocol testing, or new statistical metrics required modifying the entire monolithic file.
2. **Duplication of Statistical Functions**:
   `pgbench` implements sophisticated, highly optimized mathematical algorithms for Zipfian, Gaussian, Exponential, and Poisson random distributions, as well as fast 64-bit hashing and integer permutations. However, these implementations were trapped inside `src/bin/pgbench/pgbench.c` as `static` functions, completely inaccessible to other backend tools, test suites, or SQL queries.
3. **Inability to Generate Realistic Test Data in SQL**:
   Developers writing synthetic data generators or testing data skew (e.g. realistic e-commerce access patterns where 20% of products receive 80% of views) had to rely on cumbersome custom PL/pgSQL formulas or external Python scripts because PostgreSQL's built-in `random()` function only produces uniform distributions.

## How This Feature Solves the Problem

1. **Maintainable, Decoupled Architecture**:
   Breaking `pgbench` into modular units (`engine`, `commands`, `script`, `poller`, `variable`, `stats`, `init`) makes the codebase accessible, easy to test, and ready for future protocol enhancements (e.g. pluggable command handlers via `register_meta_command`).

2. **Common Code Reuse**:
   Moving mathematical and distribution routines to `src/common/pgbench_funcs.c` allows the same battle-tested routines to be compiled into both frontend client binaries and backend extensions without code duplication.

3. **In-Database Realistic Synthetic Data Generation**:
   With the `contrib/pgbench` extension, developers and DBAs can generate realistic Zipfian-skewed workloads, Gaussian distributions, and seeded deterministic pseudorandom permutations directly inside SQL:
   ```sql
   -- Simulate realistic skewed traffic using Zipfian distribution (s = 1.2)
   INSERT INTO orders (product_id)
   SELECT pgbench.random_zipfian(1, 100000, 1.2)
   FROM generate_series(1, 1000000);
   ```
