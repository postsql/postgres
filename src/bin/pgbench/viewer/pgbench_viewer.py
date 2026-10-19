#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
pgbench_viewer.py — Lightweight Benchmark Telemetry & Live Viewer for pgbench (--log-db)

Architecture:
1. Single-JSON-from-DB per request:
   - Phase 1 routes (/ , /live, /api/payload) execute a SINGLE PostgreSQL query:
       SELECT <schema>.pgbench_viewer_payload_json(mode, run_id, base_id, cand_id, run_ids, limit)
     which returns one self-contained JSONB document containing run selector lists,
     active running benchmarks, single-run breakdown (pgbench_show_run), groups, scripts,
     per-command stats, recalls, SLAs, time-series (progress, agg, tx), pairwise diffs
     (pgbench_compare_runs(base, cand)), and multi-run matrices (pgbench_compare_runs(bigint[])).
   - Phase 2 route (/explorer, /api/explorer) executes a SINGLE PostgreSQL query wrapping
     the user's read-only SQL into a single JSONB envelope {columns, row_count, rows},
     then invokes the Python Metatemplate Generator (generate_explorer_stpl_metatemplate)
     to dynamically synthesize a tailored bottle.SimpleTemplate (stpl) template for the
     inferred column types and render the result table + WASM chart.
2. Styling: Water.css (clean semantic HTML5 styling with dark/light mode support).
3. Minimalistic Diagram Framework (WASM):
   - Embedded 439-byte WebAssembly module (pgbench_chart.wasm) exporting:
       memory, series_min, series_max, series_mean, project_1d
   - Computes series bounds, means, and 1D/2D pixel coordinate projections in WASM
     linear memory and renders crisp SVG time-series, dual-run overlays, and HDR
     latency percentile / Recall@K bar charts.
"""

import argparse
import base64
import json
import os
import re
import subprocess
import sys
import time

# Ensure local viewer directory is on sys.path so bundled bottle.py is found
VIEWER_DIR = os.path.dirname(os.path.abspath(__file__))
if VIEWER_DIR not in sys.path:
    sys.path.insert(0, VIEWER_DIR)

import bottle
from bottle import Bottle, SimpleTemplate, request, response

# ---------------------------------------------------------------------------
# Embedded 439-byte WebAssembly Diagram Layout & Projection Module
# Exports: memory (2 pages), series_min(ptr, n)->f64, series_max(ptr, n)->f64,
#          series_mean(ptr, n)->f64, project_1d(in_ptr, n, min_v, max_v, out_start, out_span, out_ptr)
# ---------------------------------------------------------------------------
PGBENCH_CHART_WASM_B64 = (
    "AGFzbQEAAAABEQJgAn9/AXxgB39/fHx8fH8AAwUEAAAAAQUDAQACBz8FBm1lbW9yeQIACnNlcmll"
    "c19taW4AAApzZXJpZXNfbWF4AAELc2VyaWVzX21lYW4AAgpwcm9qZWN0XzFkAAMKzAIESQIBfwF8"
    "IAFFBHxEAAAAAAAAAAAFIAArAwAhA0EBIQICQANAIAIgAU8NASADIAAgAkEDdGorAwCkIQMgAkEB"
    "aiECDAALCyADCwtJAgF/AXwgAUUEfEQAAAAAAAAAAAUgACsDACEDQQEhAgJAA0AgAiABTw0BIAMg"
    "ACACQQN0aisDAKUhAyACQQFqIQIMAAsLIAMLC1ECAX8BfCABRQR8RAAAAAAAAAAABUQAAAAAAAAA"
    "ACEDQQAhAgJAA0AgAiABTw0BIAMgACACQQN0aisDAKAhAyACQQFqIQIMAAsLIAMgAbijCwtkAgF/"
    "AXwgAyACoSEIIAiZRBHqLYGZl3E9YwRARAAAAAAAAPA/IQgLQQAhBwJAA0AgByABTw0BIAYgB0ED"
    "dGogBCAAIAdBA3RqKwMAIAKhIAijIAWioDkDACAHQQFqIQcMAAsLCw=="
)
PGBENCH_CHART_WASM_BYTES = base64.b64decode(PGBENCH_CHART_WASM_B64)

# ---------------------------------------------------------------------------
# Curated SQL Presets for Phase 2 Data Explorer
# ---------------------------------------------------------------------------
EXPLORER_PRESETS = [
    {
        "id": "runs_overview",
        "title": "1. Runs Overview (pgbench_runs_v)",
        "sql": (
            "SELECT run_id, run_label, status, clients, threads, duration_sec,\n"
            "       tps, latency_avg_ms, latency_p50_ms, latency_p95_ms, latency_p99_ms,\n"
            "       recall_avg, validation_pct, sla_all_passed\n"
            "FROM pgbench_runs_v\n"
            "ORDER BY run_id DESC\n"
            "LIMIT 50"
        ),
    },
    {
        "id": "latest_run_show",
        "title": "2. Latest Run Full Breakdown (pgbench_show_run())",
        "sql": "SELECT section, scope, metric, value, unit, details FROM pgbench_show_run()",
    },
    {
        "id": "slowest_commands",
        "title": "3. Top Slowest SQL/Meta Commands Across Runs",
        "sql": (
            "SELECT c.run_id, r.run_label, c.script_idx, c.cmd_idx, c.cmd_type,\n"
            "       round(c.latency_avg_ms::numeric, 3) AS latency_avg_ms,\n"
            "       round(c.latency_max_ms::numeric, 3) AS latency_max_ms,\n"
            "       c.cnt, c.failed, c.retries, c.first_line\n"
            "FROM pgbench_run_commands c\n"
            "JOIN pgbench_runs r ON r.run_id = c.run_id\n"
            "ORDER BY c.latency_avg_ms DESC NULLS LAST\n"
            "LIMIT 40"
        ),
    },
    {
        "id": "tx_per_second_buckets",
        "title": "4. Per-Second Throughput & Tail Latency (pgbench_tx_log)",
        "sql": (
            "SELECT run_id,\n"
            "       (ts_usec / 1000000)::bigint - min((ts_usec / 1000000)::bigint) OVER (PARTITION BY run_id) AS elapsed_sec,\n"
            "       count(*) FILTER (WHERE status = 'ok') AS tps,\n"
            "       round((avg(latency_us) FILTER (WHERE status = 'ok') / 1000.0)::numeric, 3) AS latency_avg_ms,\n"
            "       round((percentile_cont(0.95) WITHIN GROUP (ORDER BY latency_us) FILTER (WHERE status = 'ok') / 1000.0)::numeric, 3) AS latency_p95_ms,\n"
            "       round((percentile_cont(0.99) WITHIN GROUP (ORDER BY latency_us) FILTER (WHERE status = 'ok') / 1000.0)::numeric, 3) AS latency_p99_ms,\n"
            "       round((avg(recall) FILTER (WHERE recall IS NOT NULL))::numeric, 4) AS recall_avg\n"
            "FROM pgbench_tx_log\n"
            "WHERE run_id = (SELECT max(run_id) FROM pgbench_tx_log)\n"
            "GROUP BY run_id, (ts_usec / 1000000)::bigint\n"
            "ORDER BY elapsed_sec ASC"
        ),
    },
    {
        "id": "recall_aging",
        "title": "5. Vector Recall@K & Index Aging Degradation",
        "sql": (
            "SELECT rc.run_id, r.run_label, rc.scope, rc.metric_name, rc.samples,\n"
            "       round(rc.avg_recall::numeric, 4) AS avg_recall,\n"
            "       round(rc.p5_recall::numeric, 4) AS p5_recall,\n"
            "       round(rc.p50_recall::numeric, 4) AS p50_recall,\n"
            "       round(rc.initial_q1::numeric, 4) AS initial_q1,\n"
            "       round(rc.mature_q4::numeric, 4) AS mature_q4,\n"
            "       round(rc.degradation_pct::numeric, 2) AS degradation_pct\n"
            "FROM pgbench_run_recalls rc\n"
            "JOIN pgbench_runs r ON r.run_id = rc.run_id\n"
            "ORDER BY rc.run_id DESC, rc.scope, rc.metric_name"
        ),
    },
    {
        "id": "live_progress_stream",
        "title": "6. Live Progress Stream (pgbench_live_progress_v)",
        "sql": (
            "SELECT run_id, run_label, status, elapsed_sec, tps,\n"
            "       latency_avg_ms, latency_stddev_ms, lag_avg_ms, failed_cnt\n"
            "FROM pgbench_live_progress_v\n"
            "ORDER BY run_id DESC, elapsed_sec ASC\n"
            "LIMIT 100"
        ),
    },
]


# ---------------------------------------------------------------------------
# Database Single-JSON Query Executor
# ---------------------------------------------------------------------------
class PgSingleJsonClient:
    """Executes a single SQL query against PostgreSQL that returns one JSON document."""

    def __init__(self, conninfo: str, schema: str = "public", psql_bin: str = "psql"):
        self.conninfo = conninfo
        self.schema = schema or "public"
        self.psql_bin = psql_bin
        self._psycopg = None
        try:
            import psycopg  # type: ignore
            self._psycopg = psycopg
        except ImportError:
            self._psycopg = None

    def _quote_ident(self, ident: str) -> str:
        return '"' + ident.replace('"', '""') + '"'

    def fetch_single_json(self, sql_returning_single_json: str, params=None) -> dict:
        """
        Executes a single SQL query returning a single JSON/JSONB scalar column
        and parses it into a Python dict.
        """
        t0 = time.monotonic()
        q_schema = self._quote_ident(self.schema)

        if self._psycopg is not None:
            with self._psycopg.connect(self.conninfo, autocommit=True) as conn:
                with conn.cursor() as cur:
                    cur.execute(f"SET search_path TO {q_schema}, pg_catalog;")
                    cur.execute(sql_returning_single_json, params)
                    row = cur.fetchone()
                    if not row or row[0] is None:
                        data = {}
                    elif isinstance(row[0], (dict, list)):
                        data = {"data": row[0]} if isinstance(row[0], list) else row[0]
                    else:
                        data = json.loads(str(row[0]))
        else:
            # Fallback to psql -X -t -A when psycopg is not installed
            rendered_sql = self._interpolate_params_for_psql(sql_returning_single_json, params)
            full_sql = f"SET search_path TO {q_schema}, pg_catalog;\n{rendered_sql};"
            cmd = [self.psql_bin, "-X", "-q", "-t", "-A", "-d", self.conninfo, "-c", full_sql]
            proc = subprocess.run(cmd, capture_output=True, text=True, check=False)
            if proc.returncode != 0:
                raise RuntimeError(proc.stderr.strip() or f"psql exited with code {proc.returncode}")
            out = proc.stdout.strip()
            lines = [ln for ln in out.splitlines() if ln.strip()]
            raw_json = lines[-1] if lines else "{}"
            data = json.loads(raw_json)

        elapsed_ms = round((time.monotonic() - t0) * 1000.0, 2)
        if isinstance(data, dict):
            data["_query_elapsed_ms"] = elapsed_ms
        return data

    def _interpolate_params_for_psql(self, sql: str, params) -> str:
        if not params:
            return sql
        out = sql
        for val in params:
            if val is None:
                rep = "NULL"
            elif isinstance(val, bool):
                rep = "TRUE" if val else "FALSE"
            elif isinstance(val, (int, float)):
                rep = str(val)
            elif isinstance(val, list):
                items = ", ".join(str(int(x)) for x in val)
                rep = f"ARRAY[{items}]::bigint[]"
            else:
                escaped = str(val).replace("'", "''")
                rep = f"'{escaped}'"
            out = out.replace("%s", rep, 1)
        return out

    def fetch_viewer_payload(
        self,
        mode: str = "overview",
        run_id=None,
        base_id=None,
        cand_id=None,
        run_ids=None,
        limit: int = 50,
    ) -> dict:
        """
        Single-SQL-query entry point for Phase 1 views:
        Calls pgbench_viewer_payload_json(...) in PostgreSQL.
        """
        sql = (
            "SELECT pgbench_viewer_payload_json(\n"
            "    %s::text,\n"
            "    %s::bigint,\n"
            "    %s::bigint,\n"
            "    %s::bigint,\n"
            "    %s::bigint[],\n"
            "    %s::int\n"
            ")::text"
        )
        return self.fetch_single_json(sql, [mode, run_id, base_id, cand_id, run_ids, limit])

    def fetch_explorer_payload(self, user_sql: str, max_rows: int = 500) -> dict:
        """
        Single-SQL-query entry point for Phase 2 Data Explorer:
        Wraps a read-only SELECT/WITH query into a single PostgreSQL JSONB query
        that returns rows + column order in a single round-trip.
        """
        cleaned = user_sql.strip().rstrip(";").strip()
        if not cleaned:
            raise ValueError("SQL query must not be empty.")
        # Guardrail: only allow read-only SELECT / WITH / TABLE / VALUES queries
        first_tok = re.split(r"\s+", cleaned, maxsplit=1)[0].upper()
        if first_tok not in ("SELECT", "WITH", "TABLE", "VALUES"):
            raise ValueError(
                f"Only read-only SELECT / WITH queries are permitted in Data Explorer (got {first_tok})."
            )

        envelope_sql = f"""
WITH _user_q AS (
    {cleaned}
),
_limited_q AS (
    SELECT * FROM _user_q LIMIT {int(max_rows)}
),
_rows_json AS (
    SELECT COALESCE(jsonb_agg(to_jsonb(q)), '[]'::jsonb) AS rows,
           count(*)::int AS row_count
    FROM _limited_q q
)
SELECT jsonb_build_object(
    'schema', current_schema(),
    'generated_at', clock_timestamp(),
    'row_count', (SELECT row_count FROM _rows_json),
    'rows', (SELECT rows FROM _rows_json)
)::text
"""
        return self.fetch_single_json(envelope_sql)


# ---------------------------------------------------------------------------
# Phase 2: Python Metatemplate Generator (produces bottle.SimpleTemplate source)
# ---------------------------------------------------------------------------
def infer_column_Descriptors(rows: list) -> list:
    """
    Inspects the JSON rows returned from PostgreSQL to classify each column's
    semantic kind so the Python metatemplate generator can emit specialized
    bottle.SimpleTemplate (stpl) formatting rules.
    """
    if not rows:
        return []
    col_names = list(rows[0].keys())
    descriptors = []
    for col in col_names:
        cl = col.lower()
        sample_vals = [r.get(col) for r in rows[:25] if r.get(col) is not None]
        py_type = "text"
        if sample_vals:
            v0 = sample_vals[0]
            if isinstance(v0, bool):
                py_type = "bool"
            elif isinstance(v0, int):
                py_type = "int"
            elif isinstance(v0, float):
                py_type = "float"
            elif isinstance(v0, (dict, list)):
                py_type = "json"
            elif isinstance(v0, str):
                # Check if numeric string
                try:
                    float(v0)
                    py_type = "float" if ("." in v0) else "int"
                except ValueError:
                    py_type = "text"

        # Semantic kind classification
        if cl == "run_id":
            kind = "run_link"
        elif cl in ("status", "verdict", "sla_all_passed", "passed", "validation_ok"):
            kind = "badge"
        elif ("pct" in cl or "recall" in cl) and py_type in ("float", "int"):
            kind = "meter_pct" if "pct" in cl else "meter_ratio"
        elif py_type in ("float", "int") and any(
            k in cl for k in ("tps", "latency", "lag", "_ms", "_us", "_sec", "delta", "val", "cnt", "samples")
        ):
            kind = "metric_num"
        elif py_type in ("float", "int"):
            kind = "num"
        elif py_type == "json":
            kind = "json"
        else:
            kind = "text"

        descriptors.append(
            {
                "name": col,
                "py_type": py_type,
                "kind": kind,
                "is_numeric": py_type in ("int", "float"),
            }
        )
    return descriptors


def generate_explorer_stpl_metatemplate(col_descriptors: list) -> str:
    """
    Python Metatemplate Generator:
    Given inferred column descriptors from a single-JSON PostgreSQL query result,
    dynamically constructs a tailored `bottle.SimpleTemplate` (stpl) template string
    that formats each column according to its semantic kind (run links, status/verdict
    badges, inline percentage/recall meters, right-aligned monospace numbers, or
    collapsible JSON trees).
    """
    if not col_descriptors:
        return "<p><em>Query returned 0 rows.</em></p>"

    lines = [
        "<!-- Dynamically Generated bottle.SimpleTemplate (stpl) via Python Metatemplate -->",
        '<div class="table-wrap">',
        '  <table class="explorer-table">',
        "    <thead>",
        "      <tr>",
    ]
    for cd in col_descriptors:
        align_cls = ' class="num-col"' if cd["is_numeric"] else ""
        lines.append(
            f'        <th{align_cls}>{cd["name"]} <small class="col-kind">({cd["kind"]})</small></th>'
        )
    lines.extend(
        [
            "      </tr>",
            "    </thead>",
            "    <tbody>",
            "      % for row in rows:",
            "      <tr>",
        ]
    )

    for cd in col_descriptors:
        col = cd["name"]
        col_repr = repr(col)
        kind = cd["kind"]

        if kind == "run_link":
            lines.append(
                f"        <td>\n"
                f"        % val = row.get({col_repr})\n"
                f"        % if val is not None:\n"
                f'          <a href="/?mode=single&run_id={{{{val}}}}"><strong>#{{{{val}}}}</strong></a>\n'
                f"        % else:\n"
                f'          <span class="muted">NULL</span>\n'
                f"        % end\n"
                f"        </td>"
            )
        elif kind == "badge":
            lines.append(
                f"        <td>\n"
                f"        % val = row.get({col_repr})\n"
                f"        % sval = str(val).upper() if val is not None else 'NULL'\n"
                f"        % if sval in ('COMPLETED', 'IMPROVED', 'PASS', 'TRUE'):\n"
                f'          <span class="badge badge-ok">{{{{val}}}}</span>\n'
                f"        % elif sval in ('RUNNING',):\n"
                f'          <span class="badge badge-live">{{{{val}}}}</span>\n'
                f"        % elif sval in ('REGRESSED', 'SLA_FAILED', 'ABORTED', 'FAIL', 'FALSE'):\n"
                f'          <span class="badge badge-err">{{{{val}}}}</span>\n'
                f"        % else:\n"
                f'          <span class="badge badge-neutral">{{{{val if val is not None else "NULL"}}}}</span>\n'
                f"        % end\n"
                f"        </td>"
            )
        elif kind in ("meter_pct", "meter_ratio"):
            scale_mult = "1.0" if kind == "meter_pct" else "100.0"
            suffix = "%" if kind == "meter_pct" else ""
            lines.append(
                f'        <td class="num-col">\n'
                f"        % val = row.get({col_repr})\n"
                f"        % if val is not None:\n"
                f"          % try:\n"
                f"            % fval = float(val)\n"
                f"            % bar_w = max(0.0, min(100.0, abs(fval) * {scale_mult}))\n"
                f'            <div class="meter-cell">'
                f'<span>{{{{f"{{fval:.4g}}{suffix}"}}}}</span>'
                f'<div class="meter-track"><div class="meter-fill" style="width:{{{{f"{{bar_w:.1f}}%"}}}}"></div></div>'
                f"</div>\n"
                f"          % except Exception:\n"
                f"            {{{{val}}}}\n"
                f"          % end\n"
                f"        % else:\n"
                f'          <span class="muted">—</span>\n'
                f"        % end\n"
                f"        </td>"
            )
        elif kind in ("metric_num", "num"):
            lines.append(
                f'        <td class="num-col">\n'
                f"        % val = row.get({col_repr})\n"
                f"        % if val is not None:\n"
                f"          % if isinstance(val, float):\n"
                f'            <code>{{{{f"{{val:,.3f}}"}}}}</code>\n'
                f"          % else:\n"
                f"            <code>{{{{val}}}}</code>\n"
                f"          % end\n"
                f"        % else:\n"
                f'          <span class="muted">—</span>\n'
                f"        % end\n"
                f"        </td>"
            )
        elif kind == "json":
            lines.append(
                f"        <td>\n"
                f"        % val = row.get({col_repr})\n"
                f"        % if val is not None:\n"
                f'          <details><summary>JSON</summary><pre class="json-inline">{{{{json.dumps(val, indent=2)}}}}</pre></details>\n'
                f"        % else:\n"
                f'          <span class="muted">NULL</span>\n'
                f"        % end\n"
                f"        </td>"
            )
        else:
            lines.append(
                f"        <td>{{{{row.get({col_repr}) if row.get({col_repr}) is not None else '—'}}}}</td>"
            )

    lines.extend(
        [
            "      </tr>",
            "      % end",
            "    </tbody>",
            "  </table>",
            "</div>",
        ]
    )
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# Main Page bottle.SimpleTemplate (Water.css + Single-JSON + WASM Plotter)
# ---------------------------------------------------------------------------
MAIN_PAGE_STPL = r"""<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>{{page_title}} — pgbench Telemetry Viewer</title>
  <style>
    /* Water.css v2 semantic core (dark/light adaptive, zero external requests) */
    :root {
      --background-body: #ffffff;
      --background: #efefef;
      --background-alt: #f7f7f7;
      --selection: #9e9e9e;
      --text-main: #1f2937;
      --text-bright: #000000;
      --text-muted: #6b7280;
      --links: #0076d1;
      --focus: rgba(0, 150, 191, 0.67);
      --border: #dbdbdb;
      --code: #000000;
      --form-text: #1d1d1d;
      --bg-card: rgba(127, 127, 127, 0.07);
      --border-subtle: rgba(127, 127, 127, 0.22);
      --accent-blue: #2563eb;
      --accent-green: #15803d;
      --accent-red: #b91c1c;
      --accent-amber: #b45309;
    }
    @media (prefers-color-scheme: dark) {
      :root {
        --background-body: #161f27;
        --background: #202b38;
        --background-alt: #1a242f;
        --selection: #1c76c5;
        --text-main: #dbdbdb;
        --text-bright: #ffffff;
        --text-muted: #a9b1ba;
        --links: #41adff;
        --focus: rgba(0, 150, 191, 0.67);
        --border: #526980;
        --code: #ffbe85;
        --form-text: #ffffff;
      }
    }
    body {
      background: var(--background-body);
      color: var(--text-main);
      line-height: 1.5;
    }
    h1, h2, h3, h4, h5, h6 { color: var(--text-bright); }
    a { color: var(--links); text-decoration: none; }
    a:hover { text-decoration: underline; }
    code, pre {
      font-family: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace;
      background: var(--background);
      color: var(--code);
      border-radius: 4px;
      padding: 2px 5px;
    }
    pre { padding: 0.75rem; overflow-x: auto; }
    input, select, textarea, button {
      color: var(--form-text);
      background-color: var(--background);
      font-family: inherit;
      font-size: 0.9rem;
      padding: 0.4rem 0.65rem;
      border: 1px solid var(--border);
      border-radius: 6px;
    }
    button, input[type="submit"] {
      background-color: #2563eb;
      color: #ffffff;
      border-color: #2563eb;
      font-weight: 600;
      cursor: pointer;
    }
    th { background: var(--background-alt); text-align: left; font-weight: 600; }
    tr:nth-child(even) { background: rgba(127, 127, 127, 0.03); }
    details {
      background: var(--background-alt);
      border: 1px solid var(--border);
      border-radius: 6px;
      padding: 0.5rem 0.75rem;
    }
    summary { cursor: pointer; font-weight: 600; }
    body {
      max-width: 1280px;
      margin: 0 auto;
      padding: 1.25rem 1.5rem 3rem;
      font-family: system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
    }
    header.top-nav {
      display: flex;
      flex-wrap: wrap;
      justify-content: space-between;
      align-items: center;
      gap: 1rem;
      border-bottom: 2px solid var(--border-subtle);
      padding-bottom: 0.75rem;
      margin-bottom: 1.25rem;
    }
    .nav-links {
      display: flex;
      gap: 0.5rem;
      flex-wrap: wrap;
      align-items: center;
    }
    .nav-pill {
      display: inline-block;
      padding: 0.35rem 0.8rem;
      border-radius: 6px;
      text-decoration: none;
      font-weight: 600;
      font-size: 0.9rem;
      border: 1px solid var(--border-subtle);
      background: var(--bg-card);
    }
    .nav-pill.active {
      background: var(--accent-blue);
      color: #fff !important;
      border-color: var(--accent-blue);
    }
    .meta-bar {
      font-size: 0.82rem;
      opacity: 0.82;
      display: flex;
      gap: 1rem;
      flex-wrap: wrap;
      margin-bottom: 1rem;
    }
    .card-grid {
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(175px, 1fr));
      gap: 0.85rem;
      margin-bottom: 1.25rem;
    }
    .stat-card {
      background: var(--bg-card);
      border: 1px solid var(--border-subtle);
      border-radius: 8px;
      padding: 0.75rem 1rem;
    }
    .stat-card .stat-label {
      font-size: 0.78rem;
      text-transform: uppercase;
      letter-spacing: 0.04em;
      opacity: 0.75;
    }
    .stat-card .stat-value {
      font-size: 1.45rem;
      font-weight: 700;
      margin-top: 0.2rem;
      font-variant-numeric: tabular-nums;
    }
    .stat-card .stat-sub {
      font-size: 0.78rem;
      opacity: 0.75;
      margin-top: 0.15rem;
    }
    .panel {
      background: var(--bg-card);
      border: 1px solid var(--border-subtle);
      border-radius: 8px;
      padding: 1rem 1.2rem;
      margin-bottom: 1.25rem;
    }
    .panel h3, .panel h4 {
      margin-top: 0;
      margin-bottom: 0.65rem;
    }
    .chart-grid {
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(380px, 1fr));
      gap: 1rem;
      margin-bottom: 1.25rem;
    }
    .chart-box {
      background: var(--bg-card);
      border: 1px solid var(--border-subtle);
      border-radius: 8px;
      padding: 0.85rem 1rem;
    }
    .chart-box h4 {
      margin: 0 0 0.4rem 0;
      font-size: 0.95rem;
      display: flex;
      justify-content: space-between;
      align-items: center;
    }
    .wasm-tag {
      font-size: 0.7rem;
      padding: 0.1rem 0.45rem;
      border-radius: 4px;
      background: rgba(37, 99, 235, 0.15);
      color: #3b82f6;
      font-weight: 600;
    }
    .badge {
      display: inline-block;
      padding: 0.15rem 0.5rem;
      border-radius: 999px;
      font-size: 0.75rem;
      font-weight: 700;
      letter-spacing: 0.02em;
    }
    .badge-ok { background: rgba(21, 128, 61, 0.18); color: #16a34a; }
    .badge-err { background: rgba(185, 28, 28, 0.18); color: #ef4444; }
    .badge-live { background: rgba(217, 119, 6, 0.22); color: #f59e0b; animation: pulse 1.6s infinite; }
    .badge-neutral { background: rgba(127, 127, 127, 0.18); }
    @keyframes pulse {
      0%, 100% { opacity: 1.0; }
      50% { opacity: 0.55; }
    }
    .table-wrap {
      overflow-x: auto;
    }
    table {
      width: 100%;
      border-collapse: collapse;
      font-size: 0.88rem;
    }
    th, td {
      padding: 0.42rem 0.6rem;
      border-bottom: 1px solid var(--border-subtle);
    }
    .num-col {
      text-align: right;
      font-variant-numeric: tabular-nums;
    }
    .col-kind {
      opacity: 0.6;
      font-weight: normal;
    }
    .meter-cell {
      display: flex;
      flex-direction: column;
      align-items: flex-end;
      gap: 2px;
    }
    .meter-track {
      width: 72px;
      height: 5px;
      background: rgba(127, 127, 127, 0.25);
      border-radius: 3px;
      overflow: hidden;
    }
    .meter-fill {
      height: 100%;
      background: var(--accent-blue);
    }
    .selector-toolbar {
      display: flex;
      gap: 0.75rem;
      flex-wrap: wrap;
      align-items: flex-end;
      margin-bottom: 0.75rem;
    }
    .selector-toolbar label {
      font-size: 0.82rem;
      margin-bottom: 0.2rem;
      display: block;
    }
    .selector-toolbar select, .selector-toolbar input, .selector-toolbar button {
      margin: 0;
    }
    .muted { opacity: 0.55; }
    .json-inline {
      font-size: 0.76rem;
      max-height: 220px;
      overflow: auto;
      margin: 0.25rem 0;
    }
    svg.wasm-svg {
      width: 100%;
      height: 230px;
      display: block;
      overflow: visible;
    }
  </style>
</head>
<body>
  <header class="top-nav">
    <div>
      <h2 style="margin:0;">pgbench Telemetry &amp; Live Benchmark Viewer</h2>
      <small class="muted">Single-JSON-from-DB &bull; bottle.py (SimpleTemplate) &bull; Water.css &bull; WASM Plotter</small>
    </div>
    <nav class="nav-links">
      <a class="nav-pill {{'active' if nav_tab == 'overview' else ''}}" href="/?mode=single">Runs &amp; Breakdown</a>
      <a class="nav-pill {{'active' if nav_tab == 'compare' else ''}}" href="/?mode=compare">Compare Runs</a>
      <a class="nav-pill {{'active' if nav_tab == 'live' else ''}}" href="/live">
        Live View
        % if active_runs and len(active_runs) > 0:
          <span class="badge badge-live">{{len(active_runs)}} active</span>
        % end
      </a>
      <a class="nav-pill {{'active' if nav_tab == 'explorer' else ''}}" href="/explorer">SQL Data Explorer (Metatemplates)</a>
      <a class="nav-pill" href="/api/payload?mode={{mode}}&run_id={{target_run_id or ''}}" target="_blank">Raw Single-JSON</a>
    </nav>
  </header>

  <div class="meta-bar">
    <span><strong>Schema:</strong> <code>{{payload.get('schema', 'public')}}</code></span>
    <span><strong>Single-Query Latency:</strong> <code>{{payload.get('_query_elapsed_ms', 0)}} ms</code></span>
    <span><strong>Generated At:</strong> <code>{{payload.get('generated_at', '')}}</code></span>
    <span><strong>WASM Engine:</strong> <code id="wasm-status-indicator">Initializing...</code></span>
  </div>

  {{!body_content}}

  <!-- Single Page JSON Payload from PostgreSQL -->
  <script id="pgbench-single-json" type="application/json">{{!payload_json_str}}</script>

  <!-- Minimalistic WebAssembly (WASM) Diagram & Charting Framework -->
  <script>
  const PGBENCH_WASM_B64 = "{{wasm_b64}}";
  let wasmExports = null;

  async function initPgbenchWasm() {
    const raw = Uint8Array.from(atob(PGBENCH_WASM_B64), c => c.charCodeAt(0));
    const mod = new WebAssembly.Module(raw);
    const inst = new WebAssembly.Instance(mod);
    wasmExports = inst.exports;
    const el = document.getElementById("wasm-status-indicator");
    if (el) {
      el.textContent = "Active (439B WASM f64 projector)";
    }
    return wasmExports;
  }

  function wasmProjectSeries(values, outStart, outSpan) {
    if (!values || values.length === 0) return { min: 0, max: 1, mean: 0, coords: [] };
    const n = values.length;
    const f64 = new Float64Array(wasmExports.memory.buffer);
    const inByteOffset = 0;
    const outByteOffset = n * 8 + 64;
    for (let i = 0; i < n; i++) {
      f64[i] = Number(values[i]) || 0.0;
    }
    const minV = wasmExports.series_min(inByteOffset, n);
    const maxV = wasmExports.series_max(inByteOffset, n);
    const meanV = wasmExports.series_mean(inByteOffset, n);
    const lo = Math.min(0, minV);
    const hi = maxV > lo ? maxV * 1.08 : lo + 1.0;
    wasmExports.project_1d(inByteOffset, n, lo, hi, outStart, outSpan, outByteOffset);
    const outIdx = outByteOffset / 8;
    const coords = Array.from(f64.slice(outIdx, outIdx + n));
    return { min: minV, max: maxV, mean: meanV, lo, hi, coords };
  }

  function renderWasmLineChart(svgId, seriesList, xKey, yKey, yUnit, colorList) {
    const svg = document.getElementById(svgId);
    if (!svg || !wasmExports) return;
    const W = 540, H = 220, padL = 52, padR = 18, padT = 18, padB = 34;
    const plotW = W - padL - padR;
    const plotH = H - padT - padB;

    const validSeries = seriesList.filter(s => s.rows && s.rows.length > 0);
    if (validSeries.length === 0) {
      svg.innerHTML = `<text x="${W/2}" y="${H/2}" text-anchor="middle" fill="#888" font-size="13">No time-series rows recorded for this run (use -P 1, -l, or --aggregate-interval)</text>`;
      return;
    }

    let allX = [], allY = [];
    validSeries.forEach(s => {
      s.rows.forEach((r, idx) => {
        allX.push(r[xKey] !== undefined && r[xKey] !== null ? Number(r[xKey]) : idx + 1);
        allY.push(Number(r[yKey]) || 0);
      });
    });

    const yStats = wasmProjectSeries(allY, padT + plotH, -plotH);
    const maxX = Math.max(...allX, 1);
    const minX = Math.min(...allX, 0);

    let html = `<svg viewBox="0 0 ${W} ${H}" class="wasm-svg">`;
    // Grid lines
    for (let g = 0; g <= 4; g++) {
      const gy = padT + (plotH * g) / 4;
      const gval = yStats.hi - ((yStats.hi - yStats.lo) * g) / 4;
      html += `<line x1="${padL}" y1="${gy.toFixed(1)}" x2="${W - padR}" y2="${gy.toFixed(1)}" stroke="rgba(127,127,127,0.2)" stroke-dasharray="3,3"/>`;
      html += `<text x="${padL - 6}" y="${(gy + 4).toFixed(1)}" text-anchor="end" font-size="10" fill="currentColor" opacity="0.7">${gval.toFixed(gval >= 100 ? 0 : 2)}</text>`;
    }

    validSeries.forEach((s, sIdx) => {
      const color = (colorList && colorList[sIdx]) || "#2563eb";
      const xs = s.rows.map((r, i) => r[xKey] !== undefined && r[xKey] !== null ? Number(r[xKey]) : i + 1);
      const ys = s.rows.map(r => Number(r[yKey]) || 0);
      const f64 = new Float64Array(wasmExports.memory.buffer);
      const n = ys.length;
      const xIn = 0, yIn = n * 8, xOut = n * 16, yOut = n * 24;
      for (let i = 0; i < n; i++) {
        f64[i] = xs[i];
        f64[n + i] = ys[i];
      }
      wasmExports.project_1d(xIn, n, minX, maxX > minX ? maxX : minX + 1, padL, plotW, xOut);
      wasmExports.project_1d(yIn, n, yStats.lo, yStats.hi, padT + plotH, -plotH, yOut);
      const px = Array.from(f64.slice(xOut / 8, xOut / 8 + n));
      const py = Array.from(f64.slice(yOut / 8, yOut / 8 + n));

      let pts = px.map((x, i) => `${x.toFixed(1)},${py[i].toFixed(1)}`).join(" ");
      if (n === 1) {
        html += `<circle cx="${px[0].toFixed(1)}" cy="${py[0].toFixed(1)}" r="4" fill="${color}"/>`;
      } else {
        const areaPts = `${px[0].toFixed(1)},${(padT + plotH).toFixed(1)} ` + pts + ` ${px[n - 1].toFixed(1)},${(padT + plotH).toFixed(1)}`;
        html += `<polygon points="${areaPts}" fill="${color}" fill-opacity="0.10"/>`;
        html += `<polyline fill="none" stroke="${color}" stroke-width="2.2" points="${pts}"/>`;
        px.forEach((x, i) => {
          html += `<circle cx="${x.toFixed(1)}" cy="${py[i].toFixed(1)}" r="2.8" fill="${color}"><title>${s.label || ''}: t=${xs[i]}s, ${ys[i]} ${yUnit}</title></circle>`;
        });
      }
    });

    html += `<text x="${padL + plotW / 2}" y="${H - 6}" text-anchor="middle" font-size="11" fill="currentColor" opacity="0.75">Elapsed Time (s) — WASM mean=${yStats.mean.toFixed(2)} ${yUnit}, max=${yStats.max.toFixed(2)} ${yUnit}</text>`;
    html += `</svg>`;
    svg.outerHTML = html.replace('<svg ', `<svg id="${svgId}" `);
  }

  function renderWasmBarChart(svgId, labels, values, unit, color) {
    const svg = document.getElementById(svgId);
    if (!svg || !wasmExports) return;
    const W = 540, H = 220, padL = 52, padR = 18, padT = 22, padB = 36;
    const plotW = W - padL - padR;
    const plotH = H - padT - padB;

    const cleanVals = (values || []).map(v => Number(v) || 0);
    if (!labels || labels.length === 0 || cleanVals.every(v => v === 0)) {
      svg.innerHTML = `<text x="${W/2}" y="${H/2}" text-anchor="middle" fill="#888" font-size="13">No percentile/bar metrics available for this run</text>`;
      return;
    }

    const stats = wasmProjectSeries(cleanVals, 0, plotH);
    const n = labels.length;
    const slotW = plotW / n;
    const barW = Math.min(54, Math.max(16, slotW * 0.62));

    let html = `<svg id="${svgId}" viewBox="0 0 ${W} ${H}" class="wasm-svg">`;
    for (let g = 0; g <= 4; g++) {
      const gy = padT + (plotH * g) / 4;
      const gval = stats.hi - (stats.hi * g) / 4;
      html += `<line x1="${padL}" y1="${gy.toFixed(1)}" x2="${W - padR}" y2="${gy.toFixed(1)}" stroke="rgba(127,127,127,0.2)" stroke-dasharray="3,3"/>`;
      html += `<text x="${padL - 6}" y="${(gy + 4).toFixed(1)}" text-anchor="end" font-size="10" fill="currentColor" opacity="0.7">${gval.toFixed(gval >= 100 ? 0 : 2)}</text>`;
    }

    for (let i = 0; i < n; i++) {
      const bh = Math.max(2, stats.coords[i]);
      const bx = padL + i * slotW + (slotW - barW) / 2;
      const by = padT + plotH - bh;
      html += `<rect x="${bx.toFixed(1)}" y="${by.toFixed(1)}" width="${barW.toFixed(1)}" height="${bh.toFixed(1)}" rx="4" fill="${color || '#2563eb'}" fill-opacity="0.85"/>`;
      html += `<text x="${(bx + barW / 2).toFixed(1)}" y="${(by - 5).toFixed(1)}" text-anchor="middle" font-size="10" font-weight="600" fill="currentColor">${cleanVals[i].toFixed(cleanVals[i] >= 100 ? 1 : 3)}</text>`;
      html += `<text x="${(bx + barW / 2).toFixed(1)}" y="${(padT + plotH + 16).toFixed(1)}" text-anchor="middle" font-size="11" fill="currentColor">${labels[i]}</text>`;
    }
    html += `</svg>`;
    svg.outerHTML = html;
  }

  function renderAllChartsFromPayload(payload) {
    if (!wasmExports || !payload) return;
    const prog = (payload.progress_series && payload.progress_series.length > 0)
      ? payload.progress_series
      : ((payload.agg_series && payload.agg_series.length > 0) ? payload.agg_series : (payload.tx_series || []));
    const tpsKey = (prog.length > 0 && prog[0].tps !== undefined) ? "tps" : ((prog.length > 0 && prog[0].cnt !== undefined) ? "cnt" : "ok_cnt");

    if (document.getElementById("wasm-chart-tps")) {
      if (payload.compare_pair && payload.compare_pair.base_progress && payload.compare_pair.cand_progress &&
          (payload.compare_pair.base_progress.length > 0 || payload.compare_pair.cand_progress.length > 0)) {
        renderWasmLineChart("wasm-chart-tps", [
          { label: "Base", rows: payload.compare_pair.base_progress },
          { label: "Candidate", rows: payload.compare_pair.cand_progress }
        ], "elapsed_sec", "tps", "tx/s", ["#64748b", "#2563eb"]);
      } else {
        renderWasmLineChart("wasm-chart-tps", [{ label: "Run", rows: prog }], "elapsed_sec", tpsKey, "tx/s", ["#2563eb"]);
      }
    }

    if (document.getElementById("wasm-chart-lat-series")) {
      renderWasmLineChart("wasm-chart-lat-series", [{ label: "Avg Latency", rows: prog }], "elapsed_sec", "latency_avg_ms", "ms", ["#16a34a"]);
    }

    if (document.getElementById("wasm-chart-hdr")) {
      const sr = payload.selected_run || {};
      const lt = payload.live_tx_summary || {};
      const pcts = [
        sr.latency_avg_ms ?? lt.avg_ms ?? 0,
        sr.latency_p50_ms ?? lt.p50_ms ?? 0,
        sr.latency_p90_ms ?? lt.p90_ms ?? 0,
        sr.latency_p95_ms ?? lt.p95_ms ?? 0,
        sr.latency_p99_ms ?? lt.p99_ms ?? 0,
        sr.latency_p99_9_ms ?? lt.p99_9_ms ?? 0
      ];
      renderWasmBarChart("wasm-chart-hdr", ["avg", "p50", "p90", "p95", "p99", "p99.9"], pcts, "ms", "#8b5cf6");
    }

    if (document.getElementById("wasm-chart-multi") && payload.compare_multi && payload.compare_multi.selected_runs) {
      const runs = payload.compare_multi.selected_runs;
      const labels = runs.map(r => r.run_label ? `#${r.run_id} ${r.run_label}` : `#${r.run_id}`);
      const vals = runs.map(r => r.tps || 0);
      renderWasmBarChart("wasm-chart-multi", labels, vals, "tx/s", "#0ea5e9");
    }

    if (document.getElementById("wasm-chart-explorer") && payload.rows && payload.rows.length > 0) {
      const rows = payload.rows;
      const keys = Object.keys(rows[0]);
      const numKeys = keys.filter(k => k !== "run_id" && rows.some(r => r[k] !== null && !isNaN(Number(r[k]))));
      if (numKeys.length > 0) {
        const yKey = numKeys.find(k => k.includes("tps") || k.includes("latency") || k.includes("recall") || k.includes("val")) || numKeys[0];
        const xKey = keys[0];
        const labels = rows.slice(0, 15).map((r, i) => String(r[xKey] ?? i + 1).slice(0, 14));
        const vals = rows.slice(0, 15).map(r => Number(r[yKey]) || 0);
        renderWasmBarChart("wasm-chart-explorer", labels, vals, yKey, "#2563eb");
      }
    }
  }

  document.addEventListener("DOMContentLoaded", async () => {
    await initPgbenchWasm();
    const el = document.getElementById("pgbench-single-json");
    if (el) {
      try {
        const payload = JSON.parse(el.textContent);
        renderAllChartsFromPayload(payload);
      } catch (e) {
        console.error("Failed to parse single JSON payload:", e);
      }
    }
  });
  </script>
</body>
</html>
"""

# ---------------------------------------------------------------------------
# Phase 1 Body Template (`bottle.SimpleTemplate` / `stpl`)
# ---------------------------------------------------------------------------
PHASE1_BODY_STPL = r"""
% runs = payload.get('runs') or []
% active_runs = payload.get('active_runs') or []
% sel = payload.get('selected_run') or {}
% show_rows = payload.get('show_run_rows') or []
% groups = payload.get('groups') or []
% scripts = payload.get('scripts') or []
% commands = payload.get('commands') or []
% recalls = payload.get('recalls') or []
% slas = payload.get('slas') or []
% pair = payload.get('compare_pair')
% multi = payload.get('compare_multi')
% live_tx = payload.get('live_tx_summary') or {}

<!-- 1. Benchmark Run Selector Panel -->
<section class="panel">
  <h3>1. Benchmark Run Selector</h3>
  <form method="get" action="/" class="selector-toolbar">
    <div>
      <label for="mode-sel">View Mode</label>
      <select id="mode-sel" name="mode">
        <option value="single" {{'selected' if mode == 'single' else ''}}>Single-Run Breakdown (pgbench_show_run)</option>
        <option value="compare" {{'selected' if mode == 'compare' else ''}}>Pairwise Diff (pgbench_compare_runs)</option>
        <option value="multi" {{'selected' if mode == 'multi' else ''}}>Multi-Run Matrix (pgbench_compare_runs(bigint[]))</option>
        <option value="live" {{'selected' if mode == 'live' else ''}}>Live Running Benchmark View</option>
      </select>
    </div>
    <div>
      <label for="run-sel">Target Run</label>
      <select id="run-sel" name="run_id">
        % for r in runs:
          <option value="{{r['run_id']}}" {{'selected' if sel and sel.get('run_id') == r['run_id'] else ''}}>
            #{{r['run_id']}} — {{r.get('run_label') or 'unlabeled'}} ({{r.get('status')}}, {{r.get('tps') or 0}} tx/s)
          </option>
        % end
      </select>
    </div>
    <div>
      <label for="base-sel">Base Run (Pairwise)</label>
      <select id="base-sel" name="base_id">
        <option value="">— Select Base —</option>
        % for r in runs:
          <option value="{{r['run_id']}}" {{'selected' if str(base_id or '') == str(r['run_id']) else ''}}>
            #{{r['run_id']}} ({{r.get('run_label') or 'unlabeled'}})
          </option>
        % end
      </select>
    </div>
    <div>
      <label for="cand-sel">Candidate Run (Pairwise)</label>
      <select id="cand-sel" name="cand_id">
        <option value="">— Select Candidate —</option>
        % for r in runs:
          <option value="{{r['run_id']}}" {{'selected' if str(cand_id or '') == str(r['run_id']) else ''}}>
            #{{r['run_id']}} ({{r.get('run_label') or 'unlabeled'}})
          </option>
        % end
      </select>
    </div>
    <div>
      <label for="multi-inp">Multi-Run IDs (comma-sep)</label>
      <input id="multi-inp" type="text" name="run_ids" value="{{run_ids_str or ''}}" placeholder="e.g. 1,2,3" style="width:130px;">
    </div>
    <div>
      <button type="submit">Apply View</button>
    </div>
  </form>

  <details {{'open' if len(runs) <= 8 else ''}}>
    <summary><strong>Recorded Runs in <code>pgbench_runs_v</code> ({{len(runs)}} runs)</strong> — check runs to compare in Multi-Run Matrix</summary>
    <div class="table-wrap" style="margin-top:0.5rem;">
      <table>
        <thead>
          <tr>
            <th>Sel</th>
            <th>Run</th>
            <th>Label</th>
            <th>Status</th>
            <th>DB / Scale</th>
            <th class="num-col">Clients/Thr</th>
            <th class="num-col">Dur (s)</th>
            <th class="num-col">TPS</th>
            <th class="num-col">Avg (ms)</th>
            <th class="num-col">p50 (ms)</th>
            <th class="num-col">p95 (ms)</th>
            <th class="num-col">p99 (ms)</th>
            <th class="num-col">Recall</th>
            <th>SLA</th>
          </tr>
        </thead>
        <tbody>
          % for r in runs:
          <tr>
            <td>
              <input type="checkbox" class="multi-run-cb" value="{{r['run_id']}}"
                onchange="const ids=Array.from(document.querySelectorAll('.multi-run-cb:checked')).map(x=>x.value);document.getElementById('multi-inp').value=ids.join(',');if(ids.length>=2)document.getElementById('mode-sel').value='multi';">
            </td>
            <td><a href="/?mode=single&run_id={{r['run_id']}}"><strong>#{{r['run_id']}}</strong></a></td>
            <td><code>{{r.get('run_label') or '—'}}</code></td>
            <td>
              % st = r.get('status') or ''
              % if st == 'completed':
                <span class="badge badge-ok">completed</span>
              % elif st == 'running':
                <span class="badge badge-live">running</span>
              % else:
                <span class="badge badge-err">{{st}}</span>
              % end
            </td>
            <td>{{r.get('target_db') or '—'}} (s={{r.get('scale') or 1}})</td>
            <td class="num-col">{{r.get('clients') or 0}} / {{r.get('threads') or 0}}</td>
            <td class="num-col">{{r.get('duration_sec') if r.get('duration_sec') is not None else '—'}}</td>
            <td class="num-col"><strong>{{r.get('tps') if r.get('tps') is not None else '—'}}</strong></td>
            <td class="num-col">{{r.get('latency_avg_ms') if r.get('latency_avg_ms') is not None else '—'}}</td>
            <td class="num-col">{{r.get('latency_p50_ms') if r.get('latency_p50_ms') is not None else '—'}}</td>
            <td class="num-col">{{r.get('latency_p95_ms') if r.get('latency_p95_ms') is not None else '—'}}</td>
            <td class="num-col">{{r.get('latency_p99_ms') if r.get('latency_p99_ms') is not None else '—'}}</td>
            <td class="num-col">{{r.get('recall_avg') if r.get('recall_avg') is not None else '—'}}</td>
            <td>
              % if r.get('sla_all_passed') is True:
                <span class="badge badge-ok">PASS</span>
              % elif r.get('sla_all_passed') is False:
                <span class="badge badge-err">FAIL</span>
              % else:
                <span class="muted">—</span>
              % end
            </td>
          </tr>
          % end
        </tbody>
      </table>
    </div>
  </details>
</section>

<!-- 2. Live View Banner / Panel (when mode == 'live' or active runs exist) -->
% if mode == 'live' or len(active_runs) > 0:
<section class="panel" id="live-benchmark-panel" style="border-left: 4px solid #f59e0b;">
  <div style="display:flex;justify-content:space-between;align-items:center;flex-wrap:wrap;gap:0.5rem;">
    <h3 style="margin:0;">
      Live Running Benchmark Monitor
      % if len(active_runs) > 0:
        <span class="badge badge-live">{{len(active_runs)}} IN-FLIGHT</span>
      % else:
        <span class="badge badge-neutral">IDLE (Showing Most Recent Run #{{sel.get('run_id') if sel else '—'}})</span>
      % end
    </h3>
    <div>
      <label style="display:inline;font-size:0.85rem;">Auto-Refresh (Single-JSON Poll):
        <select id="live-poll-interval" style="display:inline;width:auto;margin:0 0.4rem;">
          <option value="1000" {{'selected' if mode == 'live' else ''}}>Every 1s</option>
          <option value="2000">Every 2s</option>
          <option value="5000">Every 5s</option>
          <option value="0" {{'selected' if mode != 'live' else ''}}>Paused</option>
        </select>
      </label>
      <span id="live-poll-ts" class="muted" style="font-size:0.8rem;"></span>
    </div>
  </div>

  % if len(active_runs) > 0:
  <div class="table-wrap" style="margin-top:0.65rem;">
    <table>
      <thead>
        <tr>
          <th>Run ID</th>
          <th>Label</th>
          <th>Target DB</th>
          <th class="num-col">Clients/Thr</th>
          <th class="num-col">Wall Elapsed</th>
          <th class="num-col">Latest Progress Tick</th>
          <th class="num-col">Live TPS</th>
          <th class="num-col">Live Avg Lat (ms)</th>
          <th class="num-col">Failed TX</th>
        </tr>
      </thead>
      <tbody id="live-active-tbody">
        % for ar in active_runs:
        <tr>
          <td><a href="/live?run_id={{ar['run_id']}}"><strong>#{{ar['run_id']}}</strong></a></td>
          <td><code>{{ar.get('run_label') or '—'}}</code></td>
          <td>{{ar.get('target_db') or '—'}}</td>
          <td class="num-col">{{ar.get('clients') or 0}} / {{ar.get('threads') or 0}}</td>
          <td class="num-col">{{ar.get('live_elapsed_sec') or 0}} s</td>
          <td class="num-col">{{ar.get('latest_progress_sec') or 0}} s</td>
          <td class="num-col"><strong>{{ar.get('latest_tps') or '—'}}</strong></td>
          <td class="num-col">{{ar.get('latest_latency_avg_ms') or '—'}}</td>
          <td class="num-col">{{ar.get('latest_failed_cnt') or 0}}</td>
        </tr>
        % end
      </tbody>
    </table>
  </div>
  % end
</section>

<script>
(function() {
  let pollTimer = null;
  const selEl = document.getElementById("live-poll-interval");
  const tsEl = document.getElementById("live-poll-ts");
  async function pollLiveSingleJson() {
    try {
      const res = await fetch("/api/payload?mode=live&run_id={{target_run_id or ''}}");
      if (!res.ok) return;
      const data = await res.json();
      if (tsEl) tsEl.textContent = "Updated " + new Date().toLocaleTimeString();
      renderAllChartsFromPayload(data);
      if (data.selected_run) {
        const sr = data.selected_run;
        const lt = data.live_tx_summary || {};
        const tpsCard = document.getElementById("card-tps-val");
        if (tpsCard && sr.tps !== null && sr.tps !== undefined) {
          tpsCard.textContent = Number(sr.tps).toFixed(2) + " tx/s";
        } else if (tpsCard && data.progress_series && data.progress_series.length > 0) {
          const lastP = data.progress_series[data.progress_series.length - 1];
          tpsCard.textContent = Number(lastP.tps || 0).toFixed(2) + " tx/s (live)";
        }
        const cntCard = document.getElementById("card-cnt-val");
        if (cntCard) {
          cntCard.textContent = sr.transactions_cnt ?? lt.tx_ok_cnt ?? 0;
        }
      }
    } catch (e) {}
  }
  function updateTimer() {
    if (pollTimer) clearInterval(pollTimer);
    const ms = Number(selEl ? selEl.value : 0);
    if (ms > 0) pollTimer = setInterval(pollLiveSingleJson, ms);
  }
  if (selEl) {
    selEl.addEventListener("change", updateTimer);
    updateTimer();
  }
})();
</script>
% end

<!-- 3. Pairwise Run Comparison View (pgbench_compare_runs(base, cand)) -->
% if pair and pair.get('diff_rows'):
<section class="panel">
  <h3>
    Pairwise Run Comparison:
    Base #{{pair['base_run']['run_id'] if pair.get('base_run') else base_id}}
    (<code>{{pair['base_run'].get('run_label') if pair.get('base_run') else ''}}</code>)
    vs Candidate #{{pair['cand_run']['run_id'] if pair.get('cand_run') else cand_id}}
    (<code>{{pair['cand_run'].get('run_label') if pair.get('cand_run') else ''}}</code>)
  </h3>
  <div class="table-wrap">
    <table>
      <thead>
        <tr>
          <th>Scope</th>
          <th>Metric</th>
          <th>Unit</th>
          <th class="num-col">Base Value</th>
          <th class="num-col">Candidate Value</th>
          <th class="num-col">Abs Delta</th>
          <th class="num-col">% Change</th>
          <th>Verdict</th>
        </tr>
      </thead>
      <tbody>
        % for d in pair['diff_rows']:
        <tr>
          <td><code>{{d['scope']}}</code></td>
          <td><strong>{{d['metric']}}</strong></td>
          <td>{{d['unit']}}</td>
          <td class="num-col">{{f"{d['base_val']:.4f}" if isinstance(d.get('base_val'), (int, float)) else '—'}}</td>
          <td class="num-col"><strong>{{f"{d['cand_val']:.4f}" if isinstance(d.get('cand_val'), (int, float)) else '—'}}</strong></td>
          <td class="num-col">{{f"{d['abs_delta']:+.4f}" if isinstance(d.get('abs_delta'), (int, float)) else '—'}}</td>
          <td class="num-col"><strong>{{f"{d['pct_change']:+.2f}%" if isinstance(d.get('pct_change'), (int, float)) else '—'}}</strong></td>
          <td>
            % v = d.get('verdict')
            % if v == 'IMPROVED':
              <span class="badge badge-ok">IMPROVED</span>
            % elif v == 'REGRESSED':
              <span class="badge badge-err">REGRESSED</span>
            % else:
              <span class="badge badge-neutral">{{v}}</span>
            % end
          </td>
        </tr>
        % end
      </tbody>
    </table>
  </div>
</section>
% end

<!-- 4. Multi-Run Matrix Comparison View (pgbench_compare_runs(bigint[])) -->
% if multi and multi.get('matrix_rows'):
<section class="panel">
  <h3>Multi-Run Matrix Comparison (<code>pgbench_compare_runs(bigint[])</code>)</h3>
  <div class="chart-box" style="margin-bottom:1rem;">
    <h4><span>Multi-Run Throughput Comparison (TPS)</span><span class="wasm-tag">WASM Bar Projector</span></h4>
    <svg id="wasm-chart-multi" class="wasm-svg"></svg>
  </div>
  <div class="table-wrap">
    <table>
      <thead>
        <tr>
          <th>Scope</th>
          <th>Metric</th>
          <th>Unit</th>
          <th>Run</th>
          <th class="num-col">Value</th>
          <th class="num-col">Delta vs Base</th>
          <th class="num-col">% vs Base</th>
          <th>SLA</th>
        </tr>
      </thead>
      <tbody>
        % for m in multi['matrix_rows']:
        <tr>
          <td><code>{{m['scope']}}</code></td>
          <td><strong>{{m['metric']}}</strong></td>
          <td>{{m['unit']}}</td>
          <td><a href="/?mode=single&run_id={{m['run_id']}}">#{{m['run_id']}} ({{m.get('run_label') or 'unlabeled'}})</a></td>
          <td class="num-col"><strong>{{f"{m['val']:.4f}" if isinstance(m.get('val'), (int, float)) else '—'}}</strong></td>
          <td class="num-col">{{f"{m['delta_vs_base']:+.4f}" if isinstance(m.get('delta_vs_base'), (int, float)) else '—'}}</td>
          <td class="num-col">{{f"{m['pct_vs_base']:+.2f}%" if isinstance(m.get('pct_vs_base'), (int, float)) else '—'}}</td>
          <td>
            % if m.get('sla_all_passed') is True:
              <span class="badge badge-ok">PASS</span>
            % elif m.get('sla_all_passed') is False:
              <span class="badge badge-err">FAIL</span>
            % else:
              <span class="muted">—</span>
            % end
          </td>
        </tr>
        % end
      </tbody>
    </table>
  </div>
</section>
% end

<!-- 5. Selected Run Summary Cards & WASM Charts -->
% if sel:
<section>
  <h3>
    Run #{{sel.get('run_id')}}
    % if sel.get('run_label'):
      — <code>{{sel.get('run_label')}}</code>
    % end
    % st = sel.get('status') or ''
    % if st == 'completed':
      <span class="badge badge-ok">completed</span>
    % elif st == 'running':
      <span class="badge badge-live">running</span>
    % else:
      <span class="badge badge-err">{{st}}</span>
    % end
  </h3>

  <div class="card-grid">
    <div class="stat-card">
      <div class="stat-label">Throughput (TPS)</div>
      <div class="stat-value" id="card-tps-val">
        {{f"{sel['tps']:.2f} tx/s" if sel.get('tps') is not None else 'In-Flight'}}
      </div>
      <div class="stat-sub">{{sel.get('clients') or 0}} clients / {{sel.get('threads') or 0}} threads</div>
    </div>
    <div class="stat-card">
      <div class="stat-label">Transactions</div>
      <div class="stat-value" id="card-cnt-val">
        {{sel.get('transactions_cnt') if sel.get('transactions_cnt') is not None else (live_tx.get('tx_ok_cnt') or 0)}}
      </div>
      <div class="stat-sub">Failed: {{sel.get('transactions_failed') or live_tx.get('tx_err_cnt') or 0}} | Duration: {{f"{sel['duration_sec']:.2f}s" if sel.get('duration_sec') else 'live'}}</div>
    </div>
    <div class="stat-card">
      <div class="stat-label">Latency Avg / p50</div>
      <div class="stat-value">
        {{f"{sel['latency_avg_ms']:.2f}" if sel.get('latency_avg_ms') is not None else (live_tx.get('avg_ms') or '—')}} ms
      </div>
      <div class="stat-sub">p50: {{f"{sel['latency_p50_ms']:.2f}" if sel.get('latency_p50_ms') is not None else (live_tx.get('p50_ms') or '—')}} ms</div>
    </div>
    <div class="stat-card">
      <div class="stat-label">Tail Latency p95 / p99</div>
      <div class="stat-value">
        {{f"{sel['latency_p95_ms']:.2f}" if sel.get('latency_p95_ms') is not None else (live_tx.get('p95_ms') or '—')}} ms
      </div>
      <div class="stat-sub">p99: {{f"{sel['latency_p99_ms']:.2f}" if sel.get('latency_p99_ms') is not None else (live_tx.get('p99_ms') or '—')}} ms | p99.9: {{f"{sel['latency_p99_9_ms']:.2f}" if sel.get('latency_p99_9_ms') is not None else (live_tx.get('p99_9_ms') or '—')}} ms</div>
    </div>
    <div class="stat-card">
      <div class="stat-label">Recall@K / Validation</div>
      <div class="stat-value">
        {{f"{sel['recall_avg']:.4f}" if sel.get('recall_avg') is not None else (live_tx.get('recall_avg') or 'N/A')}}
      </div>
      <div class="stat-sub">Validation: {{f"{sel['validation_pct']:.1f}%" if sel.get('validation_pct') is not None else '100%'}}</div>
    </div>
    <div class="stat-card">
      <div class="stat-label">SLA &amp; Stream Health</div>
      <div class="stat-value">
        % if sel.get('sla_all_passed') is False:
          <span class="badge badge-err">SLA FAILED</span>
        % else:
          <span class="badge badge-ok">SLA PASS</span>
        % end
      </div>
      <div class="stat-sub">Dropped log rows: {{sel.get('log_db_dropped_rows') or 0}}</div>
    </div>
  </div>

  <!-- WASM Diagram Grid -->
  <div class="chart-grid">
    <div class="chart-box">
      <h4><span>Throughput Time-Series (TPS over Elapsed Sec)</span><span class="wasm-tag">WASM f64 Projector</span></h4>
      <svg id="wasm-chart-tps" class="wasm-svg"></svg>
    </div>
    <div class="chart-box">
      <h4><span>Latency Time-Series (ms over Elapsed Sec)</span><span class="wasm-tag">WASM f64 Projector</span></h4>
      <svg id="wasm-chart-lat-series" class="wasm-svg"></svg>
    </div>
    <div class="chart-box">
      <h4><span>HDR Latency Percentile Distribution (ms)</span><span class="wasm-tag">WASM Bar Projector</span></h4>
      <svg id="wasm-chart-hdr" class="wasm-svg"></svg>
    </div>
  </div>

  <!-- Structured Single-Run Breakdown via pgbench_show_run(run_id) -->
  <div class="panel">
    <h4>Structured Run Breakdown (<code>SELECT * FROM pgbench_show_run({{sel.get('run_id')}})</code>)</h4>
    <div class="table-wrap">
      <table>
        <thead>
          <tr>
            <th>Section</th>
            <th>Scope</th>
            <th>Metric</th>
            <th class="num-col">Value</th>
            <th>Unit</th>
            <th>Details</th>
          </tr>
        </thead>
        <tbody>
          % for sr in show_rows:
          <tr>
            <td><strong>{{sr.get('section')}}</strong></td>
            <td><code>{{sr.get('scope')}}</code></td>
            <td><code>{{sr.get('metric')}}</code></td>
            <td class="num-col"><strong>{{sr.get('value')}}</strong></td>
            <td>{{sr.get('unit')}}</td>
            <td class="muted">{{sr.get('details')}}</td>
          </tr>
          % end
        </tbody>
      </table>
    </div>
  </div>

  % if commands and len(commands) > 0:
  <div class="panel">
    <h4>Per-Command Latency &amp; Failure Profile (<code>pgbench_run_commands</code>)</h4>
    <div class="table-wrap">
      <table>
        <thead>
          <tr>
            <th>Script #</th>
            <th>Cmd #</th>
            <th>Type</th>
            <th class="num-col">Count</th>
            <th class="num-col">Failed</th>
            <th class="num-col">Retries</th>
            <th class="num-col">Avg (ms)</th>
            <th class="num-col">Stddev (ms)</th>
            <th class="num-col">Min (ms)</th>
            <th class="num-col">Max (ms)</th>
            <th>Statement</th>
          </tr>
        </thead>
        <tbody>
          % for c in commands:
          <tr>
            <td>#{{c.get('script_idx')}}</td>
            <td>#{{c.get('cmd_idx')}}</td>
            <td><span class="badge badge-neutral">{{c.get('cmd_type')}}</span></td>
            <td class="num-col">{{c.get('cnt') or 0}}</td>
            <td class="num-col">{{c.get('failed') or 0}}</td>
            <td class="num-col">{{c.get('retries') or 0}}</td>
            <td class="num-col"><strong>{{f"{c['latency_avg_ms']:.3f}" if isinstance(c.get('latency_avg_ms'), (int, float)) else '0.000'}}</strong></td>
            <td class="num-col">{{f"{c['latency_stddev_ms']:.3f}" if isinstance(c.get('latency_stddev_ms'), (int, float)) else '0.000'}}</td>
            <td class="num-col">{{f"{c['latency_min_ms']:.3f}" if isinstance(c.get('latency_min_ms'), (int, float)) else '0.000'}}</td>
            <td class="num-col">{{f"{c['latency_max_ms']:.3f}" if isinstance(c.get('latency_max_ms'), (int, float)) else '0.000'}}</td>
            <td><code>{{c.get('first_line') or ''}}</code></td>
          </tr>
          % end
        </tbody>
      </table>
    </div>
  </div>
  % end
</section>
% end
"""

# ---------------------------------------------------------------------------
# Phase 2 Data Explorer Body Template (`bottle.SimpleTemplate` / `stpl`)
# ---------------------------------------------------------------------------
EXPLORER_BODY_STPL = r"""
<section class="panel">
  <h3>Phase 2: SQL Data Explorer with Dynamic Python Metatemplates</h3>
  <p class="muted" style="margin-top:0;">
    Executes your read-only SQL query inside PostgreSQL as a <strong>single JSONB document</strong>
    (<code>SELECT jsonb_build_object('row_count', ..., 'rows', jsonb_agg(to_jsonb(q)))</code>),
    infers column semantics in Python, dynamically generates a specialized
    <code>bottle.SimpleTemplate</code> (<code>stpl</code>) metatemplate, and renders both the table and WASM chart.
  </p>

  <div style="margin-bottom:0.75rem;display:flex;gap:0.5rem;flex-wrap:wrap;align-items:center;">
    <strong>Presets:</strong>
    % for pr in presets:
      <a class="nav-pill {{'active' if preset_id == pr['id'] else ''}}" href="/explorer?preset={{pr['id']}}" style="font-size:0.8rem;padding:0.25rem 0.6rem;">
        {{pr['title']}}
      </a>
    % end
  </div>

  <form method="get" action="/explorer">
    <label for="sql-box"><strong>PostgreSQL Query (Read-Only <code>SELECT</code> / <code>WITH</code>):</strong></label>
    <textarea id="sql-box" name="sql" rows="6" style="font-family:monospace;font-size:0.86rem;width:100%;">{{user_sql}}</textarea>
    <div style="display:flex;gap:0.75rem;align-items:center;">
      <button type="submit">Execute Single-JSON Query &amp; Synthesize Metatemplate</button>
      <span class="muted">Rows returned: <strong>{{payload.get('row_count', 0)}}</strong></span>
    </div>
  </form>
</section>

% if error_msg:
<section class="panel" style="border-left: 4px solid #ef4444;">
  <h4 style="color:#ef4444;">Query Error</h4>
  <pre>{{error_msg}}</pre>
</section>
% end

% if payload.get('rows'):
<div class="chart-box" style="margin-bottom:1.25rem;">
  <h4><span>Automatic Explorer Metric Plot (First 15 Rows)</span><span class="wasm-tag">WASM Bar Projector</span></h4>
  <svg id="wasm-chart-explorer" class="wasm-svg"></svg>
</div>
% end

<section class="panel">
  <div style="display:flex;justify-content:space-between;align-items:center;flex-wrap:wrap;">
    <h4 style="margin:0;">Rendered Output from Synthesized <code>bottle.SimpleTemplate</code></h4>
    <details>
      <summary style="cursor:pointer;font-size:0.85rem;"><strong>Inspect Generated <code>stpl</code> Metatemplate Source ({{len(generated_stpl.splitlines())}} lines)</strong></summary>
      <pre class="json-inline" style="max-width:760px;">{{generated_stpl}}</pre>
    </details>
  </div>
  <div style="margin-top:0.75rem;">
    {{!rendered_metatemplate_html}}
  </div>
</section>
"""


# ---------------------------------------------------------------------------
# Bottle Web Application Factory
# ---------------------------------------------------------------------------
def create_viewer_app(db_client: PgSingleJsonClient) -> Bottle:
    app = Bottle()

    def _parse_int(val):
        if val is None or str(val).strip() == "":
            return None
        try:
            return int(val)
        except ValueError:
            return None

    def _parse_int_list(val):
        if not val:
            return None
        out = []
        for part in str(val).split(","):
            part = part.strip()
            if part:
                try:
                    out.append(int(part))
                except ValueError:
                    pass
        return out if out else None

    @app.route("/static/water.css")
    def serve_water_css():
        response.content_type = "text/css; charset=utf-8"
        return """
/* Water.css v2 semantic core (dark/light adaptive) */
:root {
  --background-body: #ffffff;
  --background: #efefef;
  --background-alt: #f7f7f7;
  --selection: #9e9e9e;
  --text-main: #1f2937;
  --text-bright: #000000;
  --text-muted: #6b7280;
  --links: #0076d1;
  --focus: rgba(0, 150, 191, 0.67);
  --border: #dbdbdb;
  --code: #000000;
  --button-base: #d0cfcf;
  --button-hover: #9b9b9b;
  --form-placeholder: #949494;
  --form-text: #1d1d1d;
}
@media (prefers-color-scheme: dark) {
  :root {
    --background-body: #161f27;
    --background: #202b38;
    --background-alt: #1a242f;
    --selection: #1c76c5;
    --text-main: #dbdbdb;
    --text-bright: #ffffff;
    --text-muted: #a9b1ba;
    --links: #41adff;
    --focus: rgba(0, 150, 191, 0.67);
    --border: #526980;
    --code: #ffbe85;
    --button-base: #0c151c;
    --button-hover: #040a0f;
    --form-placeholder: #a9a9a9;
    --form-text: #ffffff;
  }
}
body {
  font-family: system-ui, -apple-system, BlinkMacSystemFont, 'Segoe UI', 'Roboto', 'Oxygen', 'Ubuntu', 'Cantarell', 'Fira Sans', 'Droid Sans', 'Helvetica Neue', sans-serif;
  line-height: 1.5;
  background: var(--background-body);
  color: var(--text-main);
}
h1, h2, h3, h4, h5, h6 { color: var(--text-bright); margin-top: 1rem; margin-bottom: 0.5rem; }
a { color: var(--links); text-decoration: none; }
a:hover { text-decoration: underline; }
code, pre, kbd, samp {
  font-family: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace;
  background: var(--background);
  color: var(--code);
  border-radius: 4px;
  padding: 2px 5px;
}
pre { padding: 0.75rem; overflow-x: auto; }
input, select, textarea, button {
  color: var(--form-text);
  background-color: var(--background);
  font-family: inherit;
  font-size: 0.9rem;
  padding: 0.4rem 0.65rem;
  border: 1px solid var(--border);
  border-radius: 6px;
}
button, input[type="submit"] {
  background-color: #2563eb;
  color: #ffffff;
  border-color: #2563eb;
  font-weight: 600;
  cursor: pointer;
}
button:hover, input[type="submit"]:hover { opacity: 0.92; }
table { border-collapse: collapse; width: 100%; }
th { background: var(--background-alt); text-align: left; font-weight: 600; }
tr:nth-child(even) { background: rgba(127, 127, 127, 0.03); }
details {
  background: var(--background-alt);
  border: 1px solid var(--border);
  border-radius: 6px;
  padding: 0.5rem 0.75rem;
}
summary { cursor: pointer; font-weight: 600; }
"""

    @app.route("/static/pgbench_chart.wasm")
    def serve_wasm():
        response.content_type = "application/wasm"
        return PGBENCH_CHART_WASM_BYTES

    @app.route("/api/payload")
    def api_payload():
        mode = request.query.get("mode") or "single"
        run_id = _parse_int(request.query.get("run_id"))
        base_id = _parse_int(request.query.get("base_id"))
        cand_id = _parse_int(request.query.get("cand_id"))
        run_ids = _parse_int_list(request.query.get("run_ids"))
        limit = _parse_int(request.query.get("limit")) or 50

        payload = db_client.fetch_viewer_payload(
            mode=mode,
            run_id=run_id,
            base_id=base_id,
            cand_id=cand_id,
            run_ids=run_ids,
            limit=limit,
        )
        response.content_type = "application/json; charset=utf-8"
        return json.dumps(payload, default=str)

    @app.route("/")
    @app.route("/live")
    @app.route("/run/<path_run_id:int>")
    def index_view(path_run_id=None):
        is_live_route = request.path == "/live"
        mode = "live" if is_live_route else (request.query.get("mode") or "single")
        run_id = path_run_id or _parse_int(request.query.get("run_id"))
        base_id = _parse_int(request.query.get("base_id"))
        cand_id = _parse_int(request.query.get("cand_id"))
        run_ids_str = request.query.get("run_ids") or ""
        run_ids = _parse_int_list(run_ids_str)

        # Default pairwise comparison to the 2 most recent runs when mode == 'compare'
        payload = db_client.fetch_viewer_payload(
            mode=mode,
            run_id=run_id,
            base_id=base_id,
            cand_id=cand_id,
            run_ids=run_ids,
            limit=50,
        )
        runs = payload.get("runs") or []
        if mode == "compare" and (base_id is None or cand_id is None) and len(runs) >= 2:
            base_id = runs[1]["run_id"]
            cand_id = runs[0]["run_id"]
            payload = db_client.fetch_viewer_payload(
                mode=mode,
                run_id=run_id,
                base_id=base_id,
                cand_id=cand_id,
                run_ids=run_ids,
                limit=50,
            )
        elif mode == "multi" and not run_ids and len(runs) >= 2:
            run_ids = [r["run_id"] for r in reversed(runs[:4])]
            run_ids_str = ",".join(str(x) for x in run_ids)
            payload = db_client.fetch_viewer_payload(
                mode=mode,
                run_id=run_id,
                base_id=base_id,
                cand_id=cand_id,
                run_ids=run_ids,
                limit=50,
            )

        body_html = SimpleTemplate(PHASE1_BODY_STPL).render(
            payload=payload,
            mode=mode,
            target_run_id=payload.get("target_run_id"),
            base_id=base_id,
            cand_id=cand_id,
            run_ids_str=run_ids_str,
        )

        nav_tab = "live" if mode == "live" else ("compare" if mode in ("compare", "multi") else "overview")
        return SimpleTemplate(MAIN_PAGE_STPL).render(
            page_title="Live Benchmark View" if mode == "live" else "Benchmark Runs & Comparison",
            nav_tab=nav_tab,
            mode=mode,
            target_run_id=payload.get("target_run_id"),
            active_runs=payload.get("active_runs") or [],
            payload=payload,
            payload_json_str=json.dumps(payload, default=str),
            wasm_b64=PGBENCH_CHART_WASM_B64,
            body_content=body_html,
        )

    @app.route("/explorer")
    def explorer_view():
        preset_id = request.query.get("preset") or ""
        user_sql = request.query.get("sql") or ""
        if not user_sql:
            chosen = next((p for p in EXPLORER_PRESETS if p["id"] == preset_id), EXPLORER_PRESETS[0])
            preset_id = chosen["id"]
            user_sql = chosen["sql"]

        error_msg = None
        payload = {"schema": db_client.schema, "row_count": 0, "rows": []}
        generated_stpl = ""
        rendered_metatemplate_html = ""

        try:
            payload = db_client.fetch_explorer_payload(user_sql)
            rows = payload.get("rows") or []
            col_descriptors = infer_column_Descriptors(rows)
            generated_stpl = generate_explorer_stpl_metatemplate(col_descriptors)
            rendered_metatemplate_html = SimpleTemplate(generated_stpl).render(
                rows=rows, json=json
            )
        except Exception as exc:
            error_msg = str(exc)

        body_html = SimpleTemplate(EXPLORER_BODY_STPL).render(
            presets=EXPLORER_PRESETS,
            preset_id=preset_id,
            user_sql=user_sql,
            error_msg=error_msg,
            payload=payload,
            generated_stpl=generated_stpl,
            rendered_metatemplate_html=rendered_metatemplate_html,
        )

        return SimpleTemplate(MAIN_PAGE_STPL).render(
            page_title="SQL Data Explorer (Metatemplates)",
            nav_tab="explorer",
            mode="explorer",
            target_run_id=None,
            active_runs=[],
            payload=payload,
            payload_json_str=json.dumps(payload, default=str),
            wasm_b64=PGBENCH_CHART_WASM_B64,
            body_content=body_html,
        )

    return app


def main():
    from socketserver import ThreadingMixIn
    from wsgiref.simple_server import WSGIRequestHandler, WSGIServer, make_server

    class ThreadingWSGIServer(ThreadingMixIn, WSGIServer):
        daemon_threads = True

    parser = argparse.ArgumentParser(
        description="pgbench Telemetry & Live Benchmark Viewer (bottle.py + Water.css + WASM)"
    )
    parser.add_argument(
        "--log-db",
        "--conninfo",
        dest="log_db",
        default=os.environ.get("PGBENCH_LOG_DB", "host=localhost port=54320 dbname=pgbench_logdb_test"),
        help="PostgreSQL connection string for the pgbench telemetry database",
    )
    parser.add_argument(
        "--schema",
        default=os.environ.get("PGBENCH_LOG_DB_SCHEMA", "public"),
        help="Database schema containing pgbench_runs and telemetry tables (default: public)",
    )
    parser.add_argument("--host", default="0.0.0.0", help="HTTP bind address (default: 0.0.0.0)")
    parser.add_argument("--port", type=int, default=8094, help="HTTP listen port (default: 8094)")
    parser.add_argument("--psql-bin", default="psql", help="Fallback psql binary path")
    args = parser.parse_args()

    # Ensure pgbench_chart.wasm file exists alongside pgbench_viewer.py
    wasm_file = os.path.join(VIEWER_DIR, "pgbench_chart.wasm")
    if not os.path.exists(wasm_file):
        with open(wasm_file, "wb") as f:
            f.write(PGBENCH_CHART_WASM_BYTES)

    client = PgSingleJsonClient(args.log_db, schema=args.schema, psql_bin=args.psql_bin)
    app = create_viewer_app(client)
    print(
        f"Starting pgbench Telemetry Viewer on http://{args.host}:{args.port} "
        f"(db={args.log_db!r}, schema={args.schema!r})",
        flush=True,
    )
    with make_server(args.host, args.port, app, server_class=ThreadingWSGIServer, handler_class=WSGIRequestHandler) as httpd:
        httpd.serve_forever()


if __name__ == "__main__":
    main()
