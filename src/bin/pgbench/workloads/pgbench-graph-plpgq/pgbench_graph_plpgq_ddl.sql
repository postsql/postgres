/*
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * DISCLAIMER: This workload is an independent engineering benchmark designed
 * for database performance research and regression testing. While modeled
 * after graph traversal patterns published by the Graph Data Council (LDBC),
 * this is NOT an LDBC Benchmark, is NOT an implementation of an LDBC Benchmark,
 * and these results are NOT LDBC Benchmark Results.
 * LDBC, LDBC Benchmark, and LDBC Benchmark Results are registered trademarks
 * of the Graph Data Council. Distributed under CC-BY 4.0 attribution terms.
 */

--
-- pgbench-graph-plpgq: Property Graph Definitions for pl_pgq (SQL/PGQ over PLV8/PLJS + WebAssembly)
-- Defines social_graph() and financial_graph() using LANGUAGE "CREATE PROPERTY GRAPH".
--

SET client_min_messages = warning;

DO $$
BEGIN
    IF NOT EXISTS (SELECT 1 FROM pg_catalog.pg_language WHERE lanname IN ('plv8', 'pljs')) THEN
        IF pg_catalog.current_setting('plpgq.engine', true) = 'pljs' THEN
            CREATE EXTENSION IF NOT EXISTS pljs;
        ELSE
            CREATE EXTENSION IF NOT EXISTS plv8;
        END IF;
    END IF;
END;
$$;
CREATE EXTENSION IF NOT EXISTS pl_pgq;

CREATE OR REPLACE FUNCTION social_graph() RETURNS "PROPERTY GRAPH"
LANGUAGE "CREATE PROPERTY GRAPH" AS $$
    VERTEX TABLES (
        pgbench_graph_places KEY (id)
            LABEL Place PROPERTIES ALL COLUMNS,
        pgbench_graph_tags KEY (id)
            LABEL Tag PROPERTIES ALL COLUMNS,
        pgbench_graph_persons KEY (id)
            LABEL Person PROPERTIES ALL COLUMNS,
        pgbench_graph_forums KEY (id)
            LABEL Forum PROPERTIES ALL COLUMNS,
        pgbench_graph_messages KEY (id)
            LABEL Message PROPERTIES ALL COLUMNS
    )
    EDGE TABLES (
        pgbench_graph_knows KEY (src_id, dst_id)
            SOURCE KEY (src_id) REFERENCES pgbench_graph_persons (id)
            DESTINATION KEY (dst_id) REFERENCES pgbench_graph_persons (id)
            LABEL knows PROPERTIES ALL COLUMNS,
        pgbench_graph_person_tags KEY (person_id, tag_id)
            SOURCE KEY (person_id) REFERENCES pgbench_graph_persons (id)
            DESTINATION KEY (tag_id) REFERENCES pgbench_graph_tags (id)
            LABEL hasInterest PROPERTIES ALL COLUMNS,
        pgbench_graph_forum_members KEY (forum_id, person_id)
            SOURCE KEY (forum_id) REFERENCES pgbench_graph_forums (id)
            DESTINATION KEY (person_id) REFERENCES pgbench_graph_persons (id)
            LABEL hasMember PROPERTIES ALL COLUMNS,
        pgbench_graph_likes KEY (person_id, message_id)
            SOURCE KEY (person_id) REFERENCES pgbench_graph_persons (id)
            DESTINATION KEY (message_id) REFERENCES pgbench_graph_messages (id)
            LABEL likes PROPERTIES ALL COLUMNS,
        pgbench_graph_message_tags KEY (message_id, tag_id)
            SOURCE KEY (message_id) REFERENCES pgbench_graph_messages (id)
            DESTINATION KEY (tag_id) REFERENCES pgbench_graph_tags (id)
            LABEL hasTag PROPERTIES ALL COLUMNS,
        pgbench_graph_messages AS msg_has_creator KEY (id)
            SOURCE KEY (id) REFERENCES pgbench_graph_messages (id)
            DESTINATION KEY (creator_id) REFERENCES pgbench_graph_persons (id)
            LABEL hasCreator PROPERTIES (id, creator_id, created_ts),
        pgbench_graph_messages AS msg_reply_of KEY (id)
            SOURCE KEY (id) REFERENCES pgbench_graph_messages (id)
            DESTINATION KEY (parent_message_id) REFERENCES pgbench_graph_messages (id)
            LABEL replyOf PROPERTIES (id, parent_message_id, created_ts),
        pgbench_graph_messages AS msg_in_forum KEY (id)
            SOURCE KEY (forum_id) REFERENCES pgbench_graph_forums (id)
            DESTINATION KEY (id) REFERENCES pgbench_graph_messages (id)
            LABEL containerOf PROPERTIES (id, forum_id, created_ts),
        pgbench_graph_forums AS forum_has_moderator KEY (id)
            SOURCE KEY (id) REFERENCES pgbench_graph_forums (id)
            DESTINATION KEY (moderator_id) REFERENCES pgbench_graph_persons (id)
            LABEL hasModerator PROPERTIES (id, moderator_id, created_ts),
        pgbench_graph_persons AS person_located_in KEY (id)
            SOURCE KEY (id) REFERENCES pgbench_graph_persons (id)
            DESTINATION KEY (city_id) REFERENCES pgbench_graph_places (id)
            LABEL isLocatedIn PROPERTIES (id, city_id),
        pgbench_graph_places AS place_part_of KEY (id)
            SOURCE KEY (id) REFERENCES pgbench_graph_places (id)
            DESTINATION KEY (part_of_id) REFERENCES pgbench_graph_places (id)
            LABEL isPartOf PROPERTIES (id, part_of_id)
    )
$$;

CREATE OR REPLACE FUNCTION financial_graph() RETURNS "PROPERTY GRAPH"
LANGUAGE "CREATE PROPERTY GRAPH" AS $$
    VERTEX TABLES (
        pgbench_fin_accounts KEY (id)
            LABEL Account PROPERTIES ALL COLUMNS,
        pgbench_fin_mediums KEY (id)
            LABEL Medium PROPERTIES ALL COLUMNS,
        pgbench_fin_loans KEY (id)
            LABEL Loan PROPERTIES ALL COLUMNS
    )
    EDGE TABLES (
        pgbench_fin_transfers KEY (id)
            SOURCE KEY (src_acct) REFERENCES pgbench_fin_accounts (id)
            DESTINATION KEY (dst_acct) REFERENCES pgbench_fin_accounts (id)
            LABEL transfer PROPERTIES ALL COLUMNS,
        pgbench_fin_sign_ins KEY (medium_id, acct_id, created_ts)
            SOURCE KEY (medium_id) REFERENCES pgbench_fin_mediums (id)
            DESTINATION KEY (acct_id) REFERENCES pgbench_fin_accounts (id)
            LABEL signIn PROPERTIES ALL COLUMNS,
        pgbench_fin_loans AS loan_deposit KEY (id)
            SOURCE KEY (id) REFERENCES pgbench_fin_loans (id)
            DESTINATION KEY (acct_id) REFERENCES pgbench_fin_accounts (id)
            LABEL deposit PROPERTIES (id, borrower_id, acct_id, loan_amount, balance, created_ts)
    )
$$;
