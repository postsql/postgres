/*
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * DISCLAIMER: This workload is an independent engineering benchmark designed
 * for database performance research, development, and regression testing.
 * While inspired by industry transactional and decision support concepts,
 * this suite is NOT an official TPC Benchmark, has NOT been audited or
 * certified by the Transaction Processing Performance Council (TPC), and
 * does NOT produce official TPC Benchmark Results (such as tpmC or QphH).
 * TPC, TPC-C, TPC-E, TPC-H, TPC-DS, tpmC, and QphH are registered trademarks
 * of the Transaction Processing Performance Council.
 */

--
-- TPC-H Schema for pgbench
--
-- Requires contrib/pgbench extension for in-database parallel data generation.
--

SET client_min_messages = warning;

CREATE EXTENSION IF NOT EXISTS pgbench;

DROP TABLE IF EXISTS lineitem CASCADE;
DROP TABLE IF EXISTS orders CASCADE;
DROP TABLE IF EXISTS partsupp CASCADE;
DROP TABLE IF EXISTS part CASCADE;
DROP TABLE IF EXISTS customer CASCADE;
DROP TABLE IF EXISTS supplier CASCADE;
DROP TABLE IF EXISTS nation CASCADE;
DROP TABLE IF EXISTS region CASCADE;

CREATE TABLE region (
    r_regionkey integer PRIMARY KEY,
    r_name      char(25) NOT NULL,
    r_comment   varchar(152)
);

CREATE TABLE nation (
    n_nationkey integer PRIMARY KEY,
    n_name      char(25) NOT NULL,
    n_regionkey integer NOT NULL,
    n_comment   varchar(152)
);

CREATE TABLE supplier (
    s_suppkey   bigint PRIMARY KEY,
    s_name      char(25) NOT NULL,
    s_address   varchar(40) NOT NULL,
    s_nationkey integer NOT NULL,
    s_phone     char(15) NOT NULL,
    s_acctbal   numeric(15,2) NOT NULL,
    s_comment   varchar(101) NOT NULL
);

CREATE TABLE customer (
    c_custkey    bigint PRIMARY KEY,
    c_name       varchar(25) NOT NULL,
    c_address    varchar(40) NOT NULL,
    c_nationkey  integer NOT NULL,
    c_phone      char(15) NOT NULL,
    c_acctbal    numeric(15,2) NOT NULL,
    c_mktsegment char(10) NOT NULL,
    c_comment    varchar(117) NOT NULL
);

CREATE TABLE part (
    p_partkey     bigint PRIMARY KEY,
    p_name        varchar(55) NOT NULL,
    p_mfgr        char(25) NOT NULL,
    p_brand       char(10) NOT NULL,
    p_type        varchar(25) NOT NULL,
    p_size        integer NOT NULL,
    p_container   char(10) NOT NULL,
    p_retailprice numeric(15,2) NOT NULL,
    p_comment     varchar(23) NOT NULL
);

CREATE TABLE partsupp (
    ps_partkey    bigint NOT NULL,
    ps_suppkey    bigint NOT NULL,
    ps_availqty   integer NOT NULL,
    ps_supplycost numeric(15,2) NOT NULL,
    ps_comment    varchar(199) NOT NULL,
    PRIMARY KEY (ps_partkey, ps_suppkey)
);

CREATE TABLE orders (
    o_orderkey      bigint PRIMARY KEY,
    o_custkey       bigint NOT NULL,
    o_orderstatus   char(1) NOT NULL,
    o_totalprice    numeric(15,2) NOT NULL,
    o_orderdate     date NOT NULL,
    o_orderpriority char(15) NOT NULL,
    o_clerk         char(15) NOT NULL,
    o_shippriority  integer NOT NULL,
    o_comment       varchar(79) NOT NULL
);

CREATE TABLE lineitem (
    l_orderkey      bigint NOT NULL,
    l_partkey       bigint NOT NULL,
    l_suppkey       bigint NOT NULL,
    l_linenumber    integer NOT NULL,
    l_quantity      numeric(15,2) NOT NULL,
    l_extendedprice numeric(15,2) NOT NULL,
    l_discount      numeric(15,2) NOT NULL,
    l_tax           numeric(15,2) NOT NULL,
    l_returnflag    char(1) NOT NULL,
    l_linestatus    char(1) NOT NULL,
    l_shipdate      date NOT NULL,
    l_commitdate    date NOT NULL,
    l_receiptdate   date NOT NULL,
    l_shipinstruct  char(25) NOT NULL,
    l_shipmode      char(10) NOT NULL,
    l_comment       varchar(44) NOT NULL,
    PRIMARY KEY (l_orderkey, l_linenumber)
);

--
-- RF1: Batch Insert Refresh Function
--
CREATE OR REPLACE PROCEDURE pgbench_analytics_rf1(
    p_base_okey  bigint,
    p_batch_size integer,
    p_scale      integer
) LANGUAGE plpgsql AS $$
DECLARE
    i       integer;
    v_okey  bigint;
    v_ckey  bigint;
    v_pkey  bigint;
    v_skey  bigint;
    v_odate date;
    v_sdate date;
    v_cdate date;
    v_rdate date;
BEGIN
    FOR i IN 1 .. p_batch_size LOOP
        v_okey := p_base_okey + i;
        v_ckey := 1 + floor(random() * (300 * p_scale))::bigint;
        v_pkey := 1 + floor(random() * (400 * p_scale))::bigint;
        v_skey := 1 + floor(random() * (100 * p_scale))::bigint;
        v_odate := DATE '1995-01-01' + floor(random() * 1001)::integer;
        v_sdate := v_odate + (1 + floor(random() * 30))::integer;
        v_cdate := v_odate + 15;
        v_rdate := v_sdate + 5;

        INSERT INTO orders (
            o_orderkey, o_custkey, o_orderstatus, o_totalprice,
            o_orderdate, o_orderpriority, o_clerk, o_shippriority, o_comment
        ) VALUES (
            v_okey, v_ckey, 'O', 1250.00,
            v_odate, '3-MEDIUM', 'Clerk#' || lpad(i::text, 9, '0'), 0, 'RF1 refresh order'
        ) ON CONFLICT (o_orderkey) DO NOTHING;

        INSERT INTO lineitem (
            l_orderkey, l_partkey, l_suppkey, l_linenumber,
            l_quantity, l_extendedprice, l_discount, l_tax,
            l_returnflag, l_linestatus,
            l_shipdate, l_commitdate, l_receiptdate,
            l_shipinstruct, l_shipmode, l_comment
        ) VALUES (
            v_okey, v_pkey, v_skey, 1,
            10.00, 1250.00, 0.05, 0.02,
            'N', 'O',
            v_sdate, v_cdate, v_rdate,
            'DELIVER IN PERSON', 'AIR', 'RF1 refresh lineitem'
        ) ON CONFLICT (l_orderkey, l_linenumber) DO NOTHING;
    END LOOP;
END;
$$;

--
-- RF2: Batch Delete Refresh Function
--
CREATE OR REPLACE PROCEDURE pgbench_analytics_rf2(
    p_base_okey  bigint,
    p_batch_size integer
) LANGUAGE plpgsql AS $$
BEGIN
    DELETE FROM lineitem
     WHERE l_orderkey > p_base_okey
       AND l_orderkey <= p_base_okey + p_batch_size;

    DELETE FROM orders
     WHERE o_orderkey > p_base_okey
       AND o_orderkey <= p_base_okey + p_batch_size;
END;
$$;
