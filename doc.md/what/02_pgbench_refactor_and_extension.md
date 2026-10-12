# What: pgbench Modular Refactoring and SQL Extension

## Overview

This development comprises two complementary improvements to PostgreSQL benchmarking and simulation capabilities:

1. **Modular Architectural Refactoring of `src/bin/pgbench`**:
   The monolithic `pgbench.c` (~6,000 lines) was decomposed into clean, cohesive, and decoupled subsystem modules:
   - **`engine.c` / `engine.h`**: Core benchmark execution loop, multi-thread orchestrator, and client state machine (`TState`).
   - **`commands.c` / `commands.h`**: SQL and meta-command execution pipeline (`\set`, `\sleep`, `\shell`, `\gset`), with a pluggable meta-command dispatch registry (`register_meta_command`).
   - **`script.c` / `script.h`**: Built-in TPC-B scripts, custom script parsing, and expression AST evaluator. Decouples `chooseScript()` from client execution state.
   - **`poller.c` / `poller.h`**: Socket set polling abstraction wrapping `poll()`, `select()`, and epoll/kqueue event loops.
   - **`variable.c` / `variable.h`**: Strongly typed variable storage (`INT64`, `DOUBLE`, `BOOLEAN`, `NULL`), scoped namespaces, and literal parsing.
   - **`stats.c` / `stats.h`**: Statistical accumulators, latency tracking, percentiles, and lockless stat merging (`mergeStats()`).
   - **`init.c` / `init.h`**: Database initialization routines (`pgbench -i`), table schema creation, and partitioned table generation.

2. **Common Function Extraction (`src/common/pgbench_funcs.c`)**:
   Extracted the underlying mathematical algorithms, random distributions, hash functions, and permutation generators into common code usable across both client binaries and the backend server:
   - Random distributions: Uniform, Gaussian, Exponential, Zipfian, and Poisson.
   - Hashing algorithms: MurmurHash2 (64-bit), FNV-1a (64-bit), and default pgbench 64-bit hash.
   - Pseudorandom permutations: Fisher-Yates and linear congruential permutation generators for integer domains.

3. **`contrib/pgbench` SQL Extension**:
   Adds a standard PostgreSQL extension exposing the extracted distribution and pseudo-random functions directly to SQL queries under the `pgbench.*` schema:
   - `pgbench.random(min, max)`
   - `pgbench.random_gaussian(min, max, threshold)`
   - `pgbench.random_exponential(min, max, parameter)`
   - `pgbench.random_zipfian(min, max, parameter)`
   - `pgbench.random_poisson(center)`
   - `pgbench.hash_murmur2(val, seed)`
   - `pgbench.hash_fnv1a(val, seed)`
   - `pgbench.hash(val, seed)`
   - `pgbench.permute(val, size, seed)`
   - `pgbench.setseed(seed)`
