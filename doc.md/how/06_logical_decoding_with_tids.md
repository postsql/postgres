# How: User Guide for Logical Decoding with TIDs and REPLICA IDENTITY ROWID

## 1. Native Logical Replication Without Primary Keys

If your table does not have a primary key or unique index, configure it with `REPLICA IDENTITY ROWID`.

### Step 1: Configure Publisher
```sql
CREATE TABLE app_events (
    event_type TEXT,
    event_data JSONB,
    occurred_at TIMESTAMPTZ
);

-- Set replica identity to ROWID
ALTER TABLE app_events REPLICA IDENTITY ROWID;

-- Add table to publication
CREATE PUBLICATION pub_events FOR TABLE app_events;
```

### Step 2: Configure Subscriber with Index-Only Rowid
On the subscriber database, create the matching table with the virtual `.rowid` column and an index on it:

```sql
CREATE TABLE app_events (
    event_type TEXT,
    event_data JSONB,
    occurred_at TIMESTAMPTZ,
    .rowid TID
) WITH (index_only_rowid = true);

-- Create index on the virtual rowid column
CREATE INDEX app_events_rowid_idx ON app_events (.rowid);

-- Subscribe to publisher
CREATE SUBSCRIPTION sub_events
    CONNECTION 'host=publisher_host dbname=appdb'
    PUBLICATION pub_events;
```

#### What Happens Under the Hood:
- **Zero Disk Bloat**: Because `index_only_rowid = true`, the `.rowid` column is **omitted from physical heap storage**. Sequential scans see `.rowid` as NULL.
- **Fast Lookups**: The index `app_events_rowid_idx` indexes the row identifier, allowing $O(\log N)$ updates and deletes during CDC apply.
- **Post-Migration Drop**: When replication cutover finishes, simply drop the column:
  ```sql
  ALTER TABLE app_events DROP COLUMN .rowid;
  ```
  This is an instant metadata-only operation requiring zero table rewrites.

---

## 2. Using Physical TIDs in Logical Decoding Streams

Output plugins can read the physical `ctid` coordinates of each changed tuple.

### Reading with `test_decoding`
```sql
-- Create logical replication slot
SELECT pg_create_logical_replication_slot('my_slot', 'test_decoding');

-- Make table changes
INSERT INTO app_events VALUES ('login', '{"uid": 100}', now());

-- Fetch logical changes with include-tids option
SELECT * FROM pg_logical_slot_get_changes('my_slot', NULL, NULL,
    'include-xids', '0',
    'include-tids', '1'
);
```

#### Output Format:
```text
table public.app_events: INSERT (tid[0,1]): event_type[text]:'login' event_data[jsonb]:'{"uid": 100}' occurred_at[timestamp with time zone]:'2026-10-12 10:00:00+00'
```

---

## 3. Tracking Page Pruning and Line Pointer Changes

To receive notifications when dead tuples are pruned or redirected:

### Server Configuration (`postgresql.conf`)
```ini
# Enable WAL logging of row-level prune info for logical decoding
logical_decoding_prune_records = true
```

### Fetching Prune Events in SQL
```sql
SELECT * FROM pg_logical_slot_get_changes('my_slot', NULL, NULL,
    'include-prunes', '1'
);
```

#### Output Format:
```text
table public.app_events: PRUNE (block 0, dead 3, unused 2, redirected 1)
```
This enables real-time cache eviction and line-pointer synchronization in external indexing engines.
