# Technical Design & Implementation: Immediate Insert Visibility

## 1. Tuple Header Stamping Mechanics

Standard PostgreSQL MVCC stamps newly inserted tuples with the current transaction's active transaction ID (`GetCurrentTransactionId()`) in the tuple's `t_xmin` header field:

```c
/* Standard PostgreSQL insertion */
HeapTupleHeaderSetXmin(tuple->t_data, GetCurrentTransactionId());
HeapTupleHeaderSetCmin(tuple->t_data, cid);
tuple->t_data->t_infomask &= ~(HEAP_XMIN_COMMITTED | HEAP_XMIN_INVALID);
```

When `immediate_insert_visibility` is enabled:

```c
/* Immediate Insert Visibility insertion in heapam.c */
if (immediate_insert_visibility)
{
    /* Mark tuple as globally committed and visible across all snapshots */
    HeapTupleHeaderSetXmin(tuple->t_data, FrozenTransactionId);
    HeapTupleHeaderSetCmin(tuple->t_data, FirstCommandId);
    tuple->t_data->t_infomask |= (HEAP_XMIN_COMMITTED | HEAP_XMIN_FROZEN);
    tuple->t_data->t_infomask &= ~HEAP_XMIN_INVALID;
}
```

By marking the tuple with `HEAP_XMIN_COMMITTED | HEAP_XMIN_FROZEN`:
- Snapshot evaluation routines (`HeapTupleSatisfiesMVCC()` in `tqual.c`) evaluate the tuple as committed in the past relative to all existing and future snapshots.
- No commit status lookup in `pg_xact` (CLOG) is necessary, eliminating lock contention.

---

## 2. WAL Logging and Synchronous Flush

In `heapam_tuple_insert()`:

```c
XLogRecPtr recptr = log_heap_insert(relation, buffer, tuple);

if (immediate_insert_wal_flush)
{
    /* Force synchronous flush of WAL buffer up to the insert LSN */
    XLogFlush(recptr);
}
```

- When `immediate_insert_wal_flush = on`: `XLogFlush()` guarantees that the record is persisted to non-volatile storage before control returns to the client.
- When `immediate_insert_wal_flush = off`: standard asynchronous WAL write buffering is used, achieving maximum throughput.

---

## 3. Concurrency and Safety Considerations

1. **Isolation for UPDATE and DELETE**:
   - `heapam_tuple_update()` and `heapam_tuple_delete()` strictly require valid transaction IDs and normal MVCC locking mechanisms. `immediate_insert_visibility` does not alter delete or update visibility logic, guaranteeing that write conflicts are handled safely.
2. **Transaction Rollback Semantics**:
   - If a transaction inserts tuples with `immediate_insert_visibility = on` and subsequently issues `ROLLBACK`, the inserted tuples remain on the data pages marked as committed.
   - Applications employing immediate insert visibility treat tables as write-ahead append-only streams where rollback of already-inserted data is not performed.
