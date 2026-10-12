# How: User Guide for pgbench Refactoring and SQL Extension

## 1. Using the Modular `pgbench` Command-Line Tool

The refactored `pgbench` binary retains 100% command-line compatibility with standard PostgreSQL options, while offering improved modularity and reliability.

### Standard TPC-B Benchmark Execution
```bash
# Initialize database with scale factor 50
pgbench -i -s 50 -d mydatabase

# Run a 60-second benchmark with 16 concurrent clients and 4 worker threads
pgbench -c 16 -j 4 -T 60 -d mydatabase
```

### Writing Custom Scripts with Variables
`pgbench` scripts support custom random distribution expressions:
```sql
-- custom_script.sql
\set aid random_zipfian(1, 100000 * :scale, 1.2)
\set delta random(-5000, 5000)

BEGIN;
UPDATE pgbench_accounts SET abalance = abalance + :delta WHERE aid = :aid;
SELECT abalance FROM pgbench_accounts WHERE aid = :aid;
COMMIT;
```

Execute the custom script:
```bash
pgbench -c 8 -j 2 -f custom_script.sql -T 30 -d mydatabase
```

---

## 2. Using the `contrib/pgbench` SQL Extension

The `pgbench` SQL extension exposes random distributions and benchmarking utilities directly to SQL queries.

### Installation
```sql
CREATE EXTENSION pgbench;
```

### Function Reference and Examples

#### 1. Uniform Random (`pgbench.random`)
```sql
-- Generate integer uniformly distributed between min and max (inclusive)
SELECT pgbench.random(1, 100);
```

#### 2. Zipfian Distribution (`pgbench.random_zipfian`)
Models heavy-tailed, power-law distributions (e.g., popular products, viral posts):
```sql
-- pgbench.random_zipfian(min, max, parameter)
-- parameter = 1.0 to 1.5 is typical for web traffic
SELECT pgbench.random_zipfian(1, 10000, 1.2);
```

#### 3. Gaussian Distribution (`pgbench.random_gaussian`)
Models normal distributions centered around the midpoint with a threshold:
```sql
-- pgbench.random_gaussian(min, max, threshold)
-- Values concentrated around 50, bounded in [1, 100]
SELECT pgbench.random_gaussian(1, 100, 2.5);
```

#### 4. Exponential Distribution (`pgbench.random_exponential`)
Models time intervals or decaying activity:
```sql
-- pgbench.random_exponential(min, max, parameter)
SELECT pgbench.random_exponential(1, 1000, 3.0);
```

#### 5. Poisson Distribution (`pgbench.random_poisson`)
Models event arrivals with average arrival rate `center`:
```sql
-- pgbench.random_poisson(center)
SELECT pgbench.random_poisson(5.0);
```

#### 6. 64-bit Hashing & Permutations
```sql
-- Hash integer with seed
SELECT pgbench.hash_murmur2(12345, 42);
SELECT pgbench.hash_fnv1a(12345, 42);
SELECT pgbench.hash(12345, 42);

-- Pseudorandom permutation mapping [0..size-1] -> [0..size-1]
SELECT pgbench.permute(5, 100, 999);

-- Deterministic seeding for reproducible synthetic datasets
SELECT pgbench.setseed(123456789);
```

### Synthetic Data Generation Example
```sql
-- Generate a million synthetic transactions with Zipfian user activity
INSERT INTO user_activity (user_id, action_code, duration_ms)
SELECT
    pgbench.random_zipfian(1, 50000, 1.25),
    pgbench.random(1, 5),
    pgbench.random_gaussian(50, 5000, 3.0)
FROM generate_series(1, 1000000);
```
