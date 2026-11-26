# Known differences from duckdb

Each row is a query in this directory whose zugodb answer differs from
the duckdb answer recorded beside it (`tools/oracle.R`), and why.
`test-oracle-scalar.R` checks that each still differs, so a fix upstream
or in a patch shows up as a failing test and the row is removed.

| Query | duckdb | zugodb | Why |
| --- | --- | --- | --- |
| `diff-subnormal-literal` | `5e-324` | `zug_sql_error`: exceeds DOUBLE range | SlothDB's parser treats a subnormal literal as out of range (as `strtod()`'s `ERANGE` reports for one), though a double holds it. |
| `diff-time-cast` | the times, as `difftime` | `zug_type_error`: `TIME` not read yet | The engine's casts are right since libzugodb 0.3.0; zugodb does not read a `TIME` column yet (#43). |
| `diff-blob-escape` | `3` (a double) | `3L` | Both read the escapes since libzugodb 0.3.0; the engine's `octet_length()` is an `INTEGER`, duckdb's a `BIGINT`. |
