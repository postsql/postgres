# How: User Guide for ReadyForQuery Protocol Extension and Hook

## 1. Enabling Extended ReadyForQuery Messages

The behavior of the `ReadyForQuery` wire protocol message is controlled by the GUC parameter `ready_for_query_message`.

### Server Configuration (`postgresql.conf`)
```ini
# Options: 'plain' (default, 5-byte standard) or 'rich' (extended metadata)
ready_for_query_message = rich
```

### Session Configuration (SQL)
You can inspect or toggle the mode per session:
```sql
SHOW ready_for_query_message;

-- Enable rich ReadyForQuery messages in the current session
SET ready_for_query_message = 'rich';

-- Revert to legacy 5-byte format
SET ready_for_query_message = 'plain';
```

---

## 2. Inspecting Extended Protocol Messages

### Using `test_ready_for_query` Extension
The built-in test module `test_ready_for_query` demonstrates registering custom keys and reading protocol state:

```sql
CREATE EXTENSION test_ready_for_query;

-- Verify extension registration
SELECT test_ready_for_query_version();
```

### Decoding Wire Packets in Client Applications
When `ready_for_query_message = rich`, the server's `'Z'` packet format is:

| Offset | Type | Description |
| :--- | :--- | :--- |
| `0` | `char` | Message type `'Z'` |
| `1..4` | `int32` | Packet length (in network byte order) |
| `5` | `char` | Transaction status: `'I'` (idle), `'T'` (in transaction), `'E'` (error) |
| `6..` | Sequence | Repeated TLV key-value pairs: `[uint8 key_len][key][uint8 val_len][val]` |

#### Standard Keys Emitted by PostgreSQL:
- **`'T'` (Temporary Objects)**:
  `T:0` = No temporary objects; `T:1` = Temporary tables or temporary schemas exist.
- **`'H'` (Held Cursors)**:
  `H:0` = No open held cursors; `H:1` = One or more `WITH HOLD` cursors active.
- **`'P'` (Prepared Statements)**:
  `P:0` = No session prepared statements; `P:1` = Prepared statements exist in session.
- **`'l'` (Commit LSN)**:
  Value is an 8-byte big-endian unsigned integer representing the transaction's commit LSN.

---

## 3. Writing a C Extension Using `ReadyForQueryHook`

Extensions can append custom session telemetry to the wire protocol.

```c
#include "postgres.h"
#include "fmgr.h"
#include "tcop/dest.h"

PG_MODULE_MAGIC;

static ready_for_query_hook_type prev_rfq_hook = NULL;

static void
my_custom_rfq_hook(StringInfo buf)
{
    /* Always invoke previous hook in chain if registered */
    if (prev_rfq_hook)
        prev_rfq_hook(buf);

    /* Append custom key-value pair if not already present */
    if (!ready_for_query_has_key(buf, "my_app_id"))
    {
        const char *app_id = "tenant_42";
        ready_for_query_append_kv(buf, "my_app_id", strlen("my_app_id"),
                                  app_id, strlen(app_id));
    }
}

void
_PG_init(void)
{
    prev_rfq_hook = ReadyForQueryHook;
    ReadyForQueryHook = my_custom_rfq_hook;
}

void
_PG_fini(void)
{
    ReadyForQueryHook = prev_rfq_hook;
}
```
