# What: ReadyForQuery Wire Protocol Extension and C Hook

## Overview

The `ready_for_query_hook` patch enhances the PostgreSQL frontend/backend wire protocol message `ReadyForQuery` (`'Z'`) and provides a core C extension hook (`ready_for_query_hook`).

Under standard PostgreSQL, the `ReadyForQuery` message is a fixed 5-byte packet:
1. Byte 1: Message identifier `'Z'`.
2. Bytes 2–5: Message length `Int32(5)`.
3. Byte 6 (body): Single-byte transaction status indicator (`'I'` = Idle, `'T'` = In Transaction, `'E'` = In Failed Transaction).

This patch introduces two primary capabilities:

1. **Rich Wire Protocol Message (`ready_for_query_message = 'rich'`)**:
   When enabled, PostgreSQL extends the body of the `ReadyForQuery` message with length-prefixed key-value pairs (`[uint8 key_len][key_bytes][uint8 val_len][val_bytes]`). These convey critical session state indicators without requiring extra round-trips:
   - **`'T'`**: Temporary objects present in the current session (`'1'` if temporary tables/schemas exist, `'0'` otherwise). Automatically tracked and updated when temporary tables are created or dropped (including `DROP TABLE`, `DISCARD TEMP`, and `DISCARD ALL`).
   - **`'H'`**: Active `WITH HOLD` cursors present in the session (`'1'` or `'0'`), tracked via an $O(1)$ portal counter.
   - **`'P'`**: Session-local prepared statements present (`'1'` or `'0'`), tracked via the prepared statements hashtable.
   - **`'l'`**: Commit LSN of the most recently committed transaction in this session, serialized as an 8-byte unsigned integer in big-endian network byte order.

2. **Extensible C Hook (`ReadyForQueryHook`)**:
   Provides a C hook function pointer (`ReadyForQueryHook`) that extensions and plugins can register. During `ReadyForQuery` message generation in `src/backend/tcop/dest.c`, registered hooks are invoked with a `StringInfo` buffer, allowing custom extensions to append domain-specific key-value metadata to the wire protocol.

3. **Protocol & libpq Compatibility**:
   - `libpq` client protocol parser (`src/interfaces/libpq/fe-protocol3.c`) was updated to cleanly accept extended `ReadyForQuery` packets and ignore trailing metadata when running against a rich server.
   - `fe-trace.c` traces extended key-value pairs cleanly without reporting length-mismatch warnings.
   - Default setting remains `ready_for_query_message = 'plain'`, guaranteeing complete backwards compatibility with existing clients and connection poolers.
