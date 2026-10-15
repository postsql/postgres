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
-- pgbench-ds (TPC-DS-inspired) Data Maintenance Script
--
-- Inserts a batch of new store_sales and web_sales rows and purges expired
-- maintenance records using \for.
--

\set batch_size 4
\set base_ticket (9000000 + :client_id * 10000 + random(1, 5000))

BEGIN;
\for i 1 .. :batch_size BY 1
  \set t_num (:base_ticket + :i)
  \set d_sk random(1, 730)
  \set i_sk random_zipfian(1, 200 * :scale, 1.15)
  \set c_sk random(1, 200 * :scale)

  INSERT INTO store_sales (
      ss_sold_date_sk, ss_item_sk, ss_customer_sk, ss_cdemo_sk, ss_addr_sk,
      ss_store_sk, ss_promo_sk, ss_ticket_number, ss_quantity,
      ss_wholesale_cost, ss_list_price, ss_sales_price,
      ss_ext_discount_amt, ss_ext_sales_price, ss_ext_wholesale_cost,
      ss_ext_list_price, ss_ext_tax, ss_coupon_amt, ss_net_paid, ss_net_profit
  ) VALUES (
      :d_sk, :i_sk, :c_sk, :c_sk, :c_sk,
      1, 1, :t_num, 5,
      20.00, 40.00, 35.00,
      25.00, 175.00, 100.00,
      200.00, 12.25, 0.00, 175.00, 75.00
  ) ON CONFLICT (ss_item_sk, ss_ticket_number) DO NOTHING;

  DELETE FROM store_sales
   WHERE ss_item_sk = :i_sk AND ss_ticket_number = :t_num;
\endfor
COMMIT;
