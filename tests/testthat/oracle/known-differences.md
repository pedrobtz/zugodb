# Known differences from duckdb

Each row is a query in this directory whose zugodb answer differs from
the duckdb answer recorded beside it (`tools/oracle.R`), and why.
`test-oracle-scalar.R` checks that each still differs, so a fix upstream
or in a patch shows up as a failing test and the row is removed.

| Query | duckdb | zugodb | Why |
| --- | --- | --- | --- |
| `diff-substr-utf8` | `"é"` | `zug_type_error`: not valid UTF-8 | SlothDB's `substr()` counts bytes while `length()` counts characters, so it cuts a two-byte character in half; zugodb refuses the invalid string rather than hand R a corrupt one. A patch candidate. |
| `diff-tinyint-overflow` | error: overflow in addition of INT8 | `128L` | SlothDB's TINYINT arithmetic is not checked for overflow and widens the result. |
| `diff-subnormal-literal` | `5e-324` | `zug_sql_error`: exceeds DOUBLE range | SlothDB's parser treats a subnormal literal as out of range (as `strtod()`'s `ERANGE` reports for one), though a double holds it. |
| `diff-float-infinity` | `Inf` | `zug_sql_error`: out of range for FLOAT | Inserted into a FLOAT column, infinity fails SlothDB's range check against FLT_MAX (a SELECT of the same cast agrees); a DOUBLE column takes it. |
| `diff-time-cast` | the times, as `difftime` | `zug_type_error`: `TIME` values are wrong | SlothDB's casts give `00:00:00` for every `TIME` (#43), so zugodb refuses the column rather than return zeros (design §6). |
| `diff-blob-escape` | `3` | `9` | SlothDB keeps a `BLOB` literal's `\x` escapes as text; `unhex()` makes the bytes. |
