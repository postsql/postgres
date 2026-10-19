# `pgbench` Telemetry & Reporting Viewer (`src/bin/pgbench/viewer`)

A zero-dependency, single-file web application (`pgbench_viewer.py`) built with [`bottle.py`](file://bottle.py), [`Water.css`](https://watercss.kognise.dev/), and an embedded 439-byte WebAssembly layout/projection kernel ([`pgbench_chart.wasm`](file://pgbench_chart.wasm)). It renders historical benchmark runs, multi-run comparisons, live streaming telemetry, and interactive SQL explorations directly from a PostgreSQL `--log-db` telemetry database.

---

## 1. Architecture Overview

1. **Single-JSON-from-DB Data Flow (Phase 1)**:
   - On startup, `pgbench_viewer.py` bootstraps the idempotent helper function `<schema>.pgbench_viewer_payload_json(...)` inside the target PostgreSQL database.
   - Every Phase 1 view (`single`, `pairwise`, `matrix`, and `live`) executes a single `SELECT <schema>.pgbench_viewer_payload_json(...)` query via `psql -X -A -t -q` and receives one self-contained JSONB document containing the run selector catalog, selected run/comparison breakdowns (powered by `pgbench_show_run()` and `pgbench_compare_runs()`), time-series points, and active live-stream telemetry.
2. **Real-Time Live View via Periodic `COPY` Batch Commits**:
   - When `pgbench` runs with `--log-db` in `stream` mode (`--log-db-mode=stream`), worker threads flush and `COMMIT` batches every **2 seconds** (or **64 KB**) on their `COPY ... FROM STDIN` connections, while the main thread inserts progress snapshots (`-P <sec>`) into `pgbench_progress_log`.
   - Because `UNLOGGED` tables (`--log-db-unlogged=stream|all`) follow standard PostgreSQL MVCC visibility rules across sessions once committed, `/live` queries `<schema>.pgbench_live_progress_v` and `<schema>.pgbench_agg_log` in real time and auto-refreshes every 2 seconds while any benchmark has `status = 'running'`.
3. **Python Metatemplate SQL Data Explorer (Phase 2)**:
   - The `/explorer` tab wraps arbitrary read-only `SELECT` / `WITH` / `TABLE` / `VALUES` queries in a single PostgreSQL JSONB envelope (`{columns, row_count, rows}`).
   - A Python metatemplate generator (`generate_explorer_stpl_metatemplate()`) inspects the returned column names and runtime JSON types (`number`, `boolean`, `object`, `string`) and dynamically synthesizes a custom `bottle.SimpleTemplate` (`stpl`) template string.
   - Numeric percentage/delta columns (`*_pct`, `*delta*`) automatically receive signed green/red badge rules, primary throughput/latency metrics (`tps`, `p99_ms`, `avg_ms`, `recall_at_k`) receive inline proportional bar rules, and the generated `bottle.SimpleTemplate` source code is inspectable in an expandable `<details>` panel on the page.
4. **Minimalistic WebAssembly Diagram Kernel (`pgbench_chart.wasm`)**:
   - A 439-byte raw WebAssembly binary module (`pgbench_chart.wasm`) compiled without external toolchains exports 4 `f64` array kernels over linear memory:
     - `series_min(ptr, len) -> f64`
     - `series_max(ptr, len) -> f64`
     - `series_mean(ptr, len) -> f64`
     - `project_1d(in_ptr, out_ptr, len, out_min, out_max) -> void`
   - A lightweight (~55-line) browser JS bridge copies JSON time-series arrays into `Float64Array` views over WASM memory, invokes `project_1d` to compute pixel coordinates, and populates responsive `<svg>` line/area charts and bar charts.

---

## 2. Directory Contents

| File | Description |
| :--- | :--- |
| `pgbench_viewer.py` | Main `bottle.py` web application, PostgreSQL `pgbench_viewer_payload_json` bootstrapper, Python metatemplate generator (`generate_explorer_stpl_metatemplate`), and `Water.css` + WASM UI templates. |
| `pgbench_chart.wasm` | 439-byte WebAssembly `f64` min/max/mean & 1D coordinate projection kernel (`series_min`, `series_max`, `series_mean`, `project_1d`). |
| `bottle.py` | Vendored single-file Python WSGI micro-framework (`bottle.py`) so the viewer runs out-of-the-box with standard `python3` and `psql`. |
| `README.md` | This documentation file. |

---

## 3. Quick Start

### Step 1: Record Benchmark Runs into PostgreSQL (`--log-db`)

```bash
# 1. Baseline run with 1-second progress & aggregate intervals
pgbench -i -s 10 "host=localhost port=5432 dbname=benchdb"

pgbench -c 8 -j 4 -T 30 -P 1 --aggregate-interval=1 -l \
  --log-db="host=localhost port=5432 dbname=telemetry" \
  --log-db-unlogged=stream \
  --log-db-index \
  --run-label="pg20-baseline" \
  "host=localhost port=5432 dbname=benchdb"

# 2. Candidate run for comparison
pgbench -c 8 -j 4 -T 30 -P 1 --aggregate-interval=1 -l \
  --log-db="host=localhost port=5432 dbname=telemetry" \
  --log-db-unlogged=stream \
  --log-db-index \
  --run-label="pg20-candidate" \
  "host=localhost port=5432 dbname=benchdb"
```

### Step 2: Launch the Telemetry Viewer

```bash
python3 src/bin/pgbench/viewer/pgbench_viewer.py \
  --log-db="host=localhost port=5432 dbname=telemetry" \
  --schema=public \
  --host=0.0.0.0 \
  --port=8080
```

Then open `http://localhost:8080` in your browser.

---

## 4. Command-Line Options

| Option | Default | Description |
| :--- | :--- | :--- |
| `--log-db CONNINFO`, `--conninfo CONNINFO` | ` $PGBENCH_LOG_DB` or `dbname=postgres` | PostgreSQL connection string (`libpq` keyword/value format or `postgresql://` URI) pointing to the `--log-db` telemetry database. |
| `--schema SCHEMA` | ` $PGBENCH_LOG_SCHEMA` or `public` | Target PostgreSQL schema containing `pgbench_runs` and the `pgbench_*` views/SRFs. |
| `--host HOST` | `127.0.0.1` | HTTP bind address for the `bottle.py` WSGI server (`0.0.0.0` for remote/cloudtop access). |
| `--port PORT` | `8080` | HTTP listen port. |
| `--psql-bin PATH` | Auto-detected (`$PSQL_BIN`, `psql` in `PATH`, or `~/pg20_install/bin/psql`) | Path to the `psql` client binary used to execute JSON payload queries. |

---

## 5. Views & Endpoints

### Phase 1: Benchmark Views & Live Monitor

| Route | Mode / Parameters | Underlying SQL Interface | Description |
| :--- | :--- | :--- | :--- |
| `GET /` (`?mode=single&run_id=<id>`) | `mode=single` | `<schema>.pgbench_show_run(run_id)` + `<schema>.pgbench_progress_log` + `<schema>.pgbench_agg_log` | **Single-Run Breakdown**: KPI summary cards (TPS, latency avg/stddev/p50/p95/p99, SLA status), WASM SVG TPS & latency time-series charts, and Hierarchical Breakdown table (`SUMMARY`, `GROUP`, `SCRIPT`, `COMMAND`, `RECALL`, `SLA`). |
| `GET /?mode=pairwise&base_id=<b_id>&cand_id=<c_id>` | `mode=pairwise` | `<schema>.pgbench_compare_runs(base_id, cand_id)` | **Pairwise Run Comparison**: Side-by-side baseline vs. candidate metrics with WASM dual-bar comparisons and color-coded delta percentage badges (`tps_delta_pct`, `avg_lat_delta_pct`, `p50_lat_delta_pct`, `p95_lat_delta_pct`, `p99_lat_delta_pct`, `recall_delta`). |
| `GET /?mode=matrix&run_ids=1,2,3` | `mode=matrix` | `<schema>.pgbench_compare_runs(bigint[])` | **Multi-Run Matrix**: N-way comparison across an arbitrary array of `run_id`s, rendering JSONB maps (`tps_by_run`, `avg_lat_by_run`, `p99_lat_by_run`) and relative TPS progression against run #1. |
| `GET /live` | `mode=live` | `<schema>.pgbench_live_progress_v` + `<schema>.pgbench_agg_log` | **Live Benchmark View**: Real-time dashboard of currently executing (`status = 'running'`) benchmarks with progress bar, instantaneous & cumulative TPS/latency WASM charts, and 2-second auto-refresh when active runs are detected. |
| `GET /api/payload` | `?mode=...&run_id=...&base_id=...&cand_id=...&run_ids=...` | `<schema>.pgbench_viewer_payload_json(...)` | Returns the raw single-JSON payload directly from PostgreSQL (`application/json`). |

### Phase 2: SQL Data Explorer & WASM Charting

| Route | Description |
| :--- | :--- |
| `GET /explorer` (`?preset=1..6` or `?sql=...&x_col=...&y_col=...&chart_type=line\|bar`) | Interactive SQL Data Explorer with 6 curated one-click SQL presets (`pgbench_runs_v`, `pgbench_show_run()`, `pgbench_compare_runs()`, `pgbench_agg_log`, `pgbench_live_progress_v`, `pgbench_run_commands`), custom `SELECT`/`WITH` execution, dynamic Python metatemplate (`generate_explorer_stpl_metatemplate` $\rightarrow$ `bottle.SimpleTemplate`) HTML table rendering, inspectable generated `stpl` code, and interactive WASM SVG chart plotting (`line` or `bar`) over any numeric X/Y result columns. |
| `POST /explorer` | Form submission endpoint for custom SQL queries and chart column selections. |
| `GET /api/explorer?sql=...` | Executes the read-only SQL query and returns the JSON result envelope (`{columns, row_count, rows, generated_stpl}`). |
| `GET /static/pgbench_chart.wasm` | Serves the 439-byte WebAssembly `f64` layout/projection module (`application/wasm`). |
| `GET /static/water.css` | Serves the embedded fallback `Water.css` stylesheet if offline. |
