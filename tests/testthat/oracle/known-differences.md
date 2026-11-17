# Known differences from duckdb

Each row is a query in this directory whose zuslothdb answer differs from
the duckdb answer recorded beside it (`tools/oracle.R`), and why.
`test-oracle-scalar.R` checks that each still differs, so a fix upstream
or in a patch shows up as a failing test and the row is removed.

| Query | duckdb | zuslothdb | Why |
| --- | --- | --- | --- |
| `diff-substr-utf8` | `"é"` | `zuslothdb_type_error`: not valid UTF-8 | SlothDB's `substr()` counts bytes while `length()` counts characters, so it cuts a two-byte character in half; zuslothdb refuses the invalid string rather than hand R a corrupt one. A patch candidate. |
| `diff-tinyint-overflow` | error: overflow in addition of INT8 | `128L` | SlothDB's TINYINT arithmetic is not checked for overflow and widens the result. |
| `diff-subnormal-literal` | `5e-324` | `zuslothdb_sql_error`: exceeds DOUBLE range | SlothDB's parser treats a subnormal literal as out of range (as `strtod()`'s `ERANGE` reports for one), though a double holds it. |
| `diff-float-infinity` | `Inf` | `zuslothdb_sql_error`: out of range for FLOAT | Inserted into a FLOAT column, infinity fails SlothDB's range check against FLT_MAX (a SELECT of the same cast agrees); a DOUBLE column takes it. |
