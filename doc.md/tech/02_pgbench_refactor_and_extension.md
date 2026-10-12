# Technical Design & Implementation: pgbench Refactoring & Extension

## 1. Modular Subsystem Decomposition

The refactoring transforms `src/bin/pgbench/pgbench.c` from a monolithic procedural file into an extensible, object-oriented-in-C modular architecture:

```text
src/bin/pgbench/
├── pgbench.c       # CLI entrypoint, argument parsing, banner, exit handling
├── engine.c / .h   # Benchmark execution coordinator, thread manager, client runner
├── commands.c / .h # Meta-commands registry and SQL execution engine
├── script.c / .h   # Script AST parsing, tokenizer, expression evaluation
├── poller.c / .h   # Multiplexed socket polling (poll / select abstraction)
├── variable.c / .h # Scoped variable store and typed values
├── stats.c / .h    # Latency histograms, statistical accumulators, and stats merge
└── init.c / .h     # Database initialization (-i) and schema generator
```

---

## 2. Pluggable Meta-Command Registry

Meta-commands (`\set`, `\sleep`, `\shell`, etc.) are now managed through a command dispatch registry:

```c
typedef struct MetaCommandDef {
    const char *name;
    MetaCommandHandler handler;
    bool modifies_state;
} MetaCommandDef;

void register_meta_command(const MetaCommandDef *cmd_def);
bool dispatch_meta_command(TState *state, Command *command);
```
This enables test plugins or future extensions to register custom meta-commands without modifying core command evaluation loops.

---

## 3. Common Library Extraction (`src/common/pgbench_funcs.c`)

The mathematical distribution algorithms were abstracted to `src/common/pgbench_funcs.c` with declarations in `src/include/common/pgbench_funcs.h`:

### Mathematical Algorithms
1. **Zipfian Distribution**:
   Implements an exact, normalized inverse-CDF transformation and rejection-inversion algorithm with harmonic numbers ($H_{n,s}$) precomputed or approximated, ensuring bounded generation times even for large value domains ($N = 10^9$).
2. **Gaussian Distribution**:
   Implements the Box-Muller transform producing standard normally distributed pairs, scaled by a threshold parameter and centered at the domain midpoint.
3. **Exponential Distribution**:
   Calculates $-\ln(U) / \lambda$ with boundaries truncated to the user-supplied interval.
4. **Poisson Distribution**:
   Implements Knuth's algorithm for small $\lambda$ and Gaussian approximation for large $\lambda$.
5. **Permutations (`pg_permute`)**:
   Implements pseudorandom permutation generation using pseudorandom block ciphers and linear congruential generator (LCG) cycle structures, mapping $[0..N-1]$ bijectively onto itself with zero memory allocation.

---

## 4. `contrib/pgbench` SQL Extension Architecture

The `contrib/pgbench` SQL extension wraps `src/common/pgbench_funcs.c` into PostgreSQL C-language functions (`PG_FUNCTION_INFO_V1`):

```c
#include "postgres.h"
#include "fmgr.h"
#include "common/pgbench_funcs.h"

PG_FUNCTION_INFO_V1(pgbench_random_zipfian);
Datum
pgbench_random_zipfian(PG_FUNCTION_ARGS)
{
    int64 min = PG_GETARG_INT64(0);
    int64 max = PG_GETARG_INT64(1);
    float8 param = PG_GETARG_FLOAT8(2);

    if (max < min)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("max must be greater than or equal to min")));

    int64 result = pgbench_get_zipfian(min, max, param);
    PG_RETURN_INT64(result);
}
```

### Build Integration
- Shared object links directly against `src/common/pgbench_funcs.o`.
- Builds seamlessly with both GNU Make (`Makefile`) and Meson (`meson.build`).
