#!/usr/bin/env python3
"""
Migration utility for legacy pgbench benchmark telemetry and configuration files.

Updates:
1. --json-report output files: renames 'tpmC' -> 'nopm', 'QphH' -> 'qph', and legacy test names.
2. .pgbench.json configuration files: updates workload directories, script filenames, and property graph names.
3. --log-db PostgreSQL databases: updates pgbench_runs.report JSONB columns and script telemetry records.
"""

import argparse
import json
import os
import re
import sys
import subprocess

NAME_MAPPINGS = [
    # Workload directories
    ("workloads/pgbench-b/", "workloads/pgbench-banking/"),
    ("workloads/pgbench-c/", "workloads/pgbench-order/"),
    ("workloads/pgbench-e/", "workloads/pgbench-brokerage/"),
    ("workloads/pgbench-h/", "workloads/pgbench-analytics/"),
    ("workloads/pgbench-ds/", "workloads/pgbench-retail/"),

    # Filename prefixes
    ("pgbench_b_", "pgbench_banking_"),
    ("pgbench_c_", "pgbench_order_"),
    ("pgbench_e_", "pgbench_brokerage_"),
    ("pgbench_h_", "pgbench_analytics_"),
    ("pgbench_ds_", "pgbench_retail_"),

    # Graph SQL scripts
    ("pgbench_ldbc_lsqb.sql", "pgbench_graph_subgraph.sql"),
    ("pgbench_ldbc_snb_is.sql", "pgbench_graph_social_short.sql"),
    ("pgbench_ldbc_snb_ic.sql", "pgbench_graph_social_complex.sql"),
    ("pgbench_ldbc_snb_iu.sql", "pgbench_graph_social_insert.sql"),
    ("pgbench_ldbc_snb_id.sql", "pgbench_graph_social_delete.sql"),
    ("pgbench_ldbc_snb_bi.sql", "pgbench_graph_social_bi.sql"),
    ("pgbench_ldbc_finbench.sql", "pgbench_graph_financial.sql"),
    ("pgbench_ldbc_graphalytics.sql", "pgbench_graph_algorithms.sql"),

    # Graph SQL_UD scripts
    ("pgbench_ldbc_sql_ud_lsqb.sql", "pgbench_graph_sql_ud_subgraph.sql"),
    ("pgbench_ldbc_sql_ud_snb_is.sql", "pgbench_graph_sql_ud_social_short.sql"),
    ("pgbench_ldbc_sql_ud_snb_ic.sql", "pgbench_graph_sql_ud_social_complex.sql"),
    ("pgbench_ldbc_sql_ud_snb_iu.sql", "pgbench_graph_sql_ud_social_insert.sql"),
    ("pgbench_ldbc_sql_ud_snb_id.sql", "pgbench_graph_sql_ud_social_delete.sql"),
    ("pgbench_ldbc_sql_ud_snb_bi.sql", "pgbench_graph_sql_ud_social_bi.sql"),
    ("pgbench_ldbc_sql_ud_finbench.sql", "pgbench_graph_sql_ud_financial.sql"),
    ("pgbench_ldbc_sql_ud_graphalytics.sql", "pgbench_graph_sql_ud_algorithms.sql"),

    # Graph PGQ scripts
    ("pgbench_ldbc_pgq_lsqb.sql", "pgbench_graph_pgq_subgraph.sql"),
    ("pgbench_ldbc_pgq_snb_is.sql", "pgbench_graph_pgq_social_short.sql"),
    ("pgbench_ldbc_pgq_snb_ic.sql", "pgbench_graph_pgq_social_complex.sql"),
    ("pgbench_ldbc_pgq_snb_iu.sql", "pgbench_graph_pgq_social_insert.sql"),
    ("pgbench_ldbc_pgq_snb_id.sql", "pgbench_graph_pgq_social_delete.sql"),
    ("pgbench_ldbc_pgq_snb_bi.sql", "pgbench_graph_pgq_social_bi.sql"),
    ("pgbench_ldbc_pgq_finbench.sql", "pgbench_graph_pgq_financial.sql"),
    ("pgbench_ldbc_pgq_graphalytics.sql", "pgbench_graph_pgq_algorithms.sql"),

    # Graph plPGQ scripts
    ("pgbench_ldbc_plpgq_lsqb.sql", "pgbench_graph_plpgq_subgraph.sql"),
    ("pgbench_ldbc_plpgq_snb_is.sql", "pgbench_graph_plpgq_social_short.sql"),
    ("pgbench_ldbc_plpgq_snb_ic.sql", "pgbench_graph_plpgq_social_complex.sql"),
    ("pgbench_ldbc_plpgq_snb_iu.sql", "pgbench_graph_plpgq_social_insert.sql"),
    ("pgbench_ldbc_plpgq_snb_id.sql", "pgbench_graph_plpgq_social_delete.sql"),
    ("pgbench_ldbc_plpgq_snb_bi.sql", "pgbench_graph_plpgq_social_bi.sql"),
    ("pgbench_ldbc_plpgq_finbench.sql", "pgbench_graph_plpgq_financial.sql"),
    ("pgbench_ldbc_plpgq_graphalytics.sql", "pgbench_graph_plpgq_algorithms.sql"),

    # Graph JSON configs
    ("lsqb.pgbench.json", "subgraph.pgbench.json"),
    ("snb_interactive.pgbench.json", "social_interactive.pgbench.json"),
    ("snb_bi.pgbench.json", "social_bi.pgbench.json"),
    ("finbench.pgbench.json", "financial.pgbench.json"),
    ("graphalytics.pgbench.json", "algorithms.pgbench.json"),

    # Property graphs
    ("snb_graph", "social_graph"),
    ("finbench_graph", "financial_graph"),

    # Generator functions
    ("ldbc_correlated_edge", "community_correlated_edge"),
    ("ldbc_flashmob_ts", "burst_timestamp"),
]

def migrate_json_report(filepath):
    """Update a JSON report file: rename tpmC -> nopm, QphH -> qph."""
    with open(filepath, "r", encoding="utf-8") as f:
        data = json.load(f)

    changed = False

    def transform_obj(obj):
        nonlocal changed
        if isinstance(obj, dict):
            new_obj = {}
            for k, v in obj.items():
                new_k = k
                if k == "tpmC":
                    new_k = "nopm"
                    changed = True
                elif k == "QphH":
                    new_k = "qph"
                    changed = True
                new_obj[new_k] = transform_obj(v)
            return new_obj
        elif isinstance(obj, list):
            return [transform_obj(elem) for elem in obj]
        elif isinstance(obj, str):
            res = obj
            for old, new in NAME_MAPPINGS:
                if old in res:
                    res = res.replace(old, new)
                    changed = True
            return res
        return obj

    new_data = transform_obj(data)
    if changed:
        with open(filepath, "w", encoding="utf-8") as f:
            json.dump(new_data, f, indent=2)
            f.write("\n")
        print(f"Migrated report: {filepath}")
    else:
        print(f"No changes needed: {filepath}")


def migrate_config_file(filepath):
    """Update a .pgbench.json configuration file: rewrite script and suite paths."""
    with open(filepath, "r", encoding="utf-8") as f:
        content = f.read()

    new_content = content
    for old, new in NAME_MAPPINGS:
        new_content = new_content.replace(old, new)

    if new_content != content:
        with open(filepath, "w", encoding="utf-8") as f:
            f.write(new_content)
        print(f"Migrated config: {filepath}")
    else:
        print(f"No changes needed: {filepath}")


def migrate_database(conninfo):
    """Execute in-place SQL migration on a PostgreSQL --log-db database."""
    sql = """
    DO $$
    DECLARE
        migrated_runs int := 0;
    BEGIN
        IF EXISTS (SELECT 1 FROM information_schema.tables WHERE table_name = 'pgbench_runs') THEN
            UPDATE pgbench_runs
               SET report = (report - 'tpmC' - 'QphH')
                            || (CASE WHEN report ? 'tpmC' THEN jsonb_build_object('nopm', report->'tpmC') ELSE '{}'::jsonb END)
                            || (CASE WHEN report ? 'QphH' THEN jsonb_build_object('qph', report->'QphH') ELSE '{}'::jsonb END)
             WHERE report ? 'tpmC' OR report ? 'QphH';
            GET DIAGNOSTICS migrated_runs = ROW_COUNT;
            RAISE NOTICE 'pgbench_runs: migrated % run records from tpmC/QphH to nopm/qph', migrated_runs;
        END IF;

        IF EXISTS (SELECT 1 FROM information_schema.tables WHERE table_name = 'pgbench_run_scripts') THEN
            UPDATE pgbench_run_scripts
               SET script_name = REPLACE(script_name, 'pgbench_ldbc_', 'pgbench_graph_')
             WHERE script_name LIKE '%pgbench_ldbc_%';
        END IF;
    END;
    $$;
    """
    cmd = ["psql", conninfo, "-v", "ON_ERROR_STOP=1", "-c", sql]
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        print(f"Error migrating database: {res.stderr}", file=sys.stderr)
        sys.exit(1)
    print(res.stdout or "Database migration complete.")


def main():
    parser = argparse.ArgumentParser(description="Migrate legacy benchmark telemetry and configs.")
    parser.add_argument("--report", action="append", help="Path to --json-report file to migrate")
    parser.add_argument("--config", action="append", help="Path to .pgbench.json file to migrate")
    parser.add_argument("--db", help="PostgreSQL connection string (conninfo) for --log-db migration")
    args = parser.parse_args()

    if args.report:
        for r in args.report:
            migrate_json_report(r)
    if args.config:
        for c in args.config:
            migrate_config_file(c)
    if args.db:
        migrate_database(args.db)

    if not args.report and not args.config and not args.db:
        parser.print_help()


if __name__ == "__main__":
    main()
