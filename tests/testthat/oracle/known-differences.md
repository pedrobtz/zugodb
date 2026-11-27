# Known differences from duckdb

Each row is a query in this directory whose zugodb answer differs from
the duckdb answer recorded beside it (`tools/oracle.R`), and why.
`test-oracle-scalar.R` checks that each still differs, so a fix upstream
or in a patch shows up as a failing test and the row is removed.

The first table lists answers that differ. The second lists queries
duckdb answers and zugodb refuses with a `zug_sql_error`, which Q18
allows: an error, never a wrong answer. `test-oracle-sql.R` checks that
each is still refused, so a shape the engine learns moves to a `sql-`
query with its recorded answer.

| Query | duckdb | zugodb | Why |
| --- | --- | --- | --- |
| `diff-subnormal-literal` | `5e-324` | `zug_sql_error`: exceeds DOUBLE range | SlothDB's parser treats a subnormal literal as out of range (as `strtod()`'s `ERANGE` reports for one), though a double holds it. |
| `diff-time-cast` | the times, as `difftime` | `zug_type_error`: `TIME` not read yet | The engine's casts are right since libzugodb 0.3.0; zugodb does not read a `TIME` column yet (#43). |
| `diff-blob-escape` | `3` (a double) | `3L` | Both read the escapes since libzugodb 0.3.0; the engine's `octet_length()` is an `INTEGER`, duckdb's a `BIGINT`. |
| `diff-column-name` | columns `(1 + 1)` and `count_star()` | both columns `expr` | The engine names every column without an alias `expr`, so two of them share a name in the data frame. Alias each expression. |

## Refused

| Query | Refused by | What the engine lacks |
| --- | --- | --- |
| `refused-keyword-alias` | the parser | A keyword (`first`) as a column alias. |
| `refused-exists-correlated` | the binder | A correlated `EXISTS`: the outer table is not in scope. |
| `refused-scalar-correlated` | the binder | A correlated scalar subquery, for the same reason. |
| `refused-setop-nested` | the binder | A derived table over a `UNION ALL` of derived set operations (`Table '' not found`). |
| `refused-cte-ctas` | the binder | `CREATE TABLE ... AS` over a `WITH` clause. |
| `refused-generate-series-ctas` | the binder | `CREATE TABLE ... AS` over a table function. |
| `refused-window-frame` | the parser | A window frame (`ROWS BETWEEN`). |
| `refused-window-nulls` | the parser | `NULLS FIRST` or `NULLS LAST` in a window's `ORDER BY`. |
| `refused-window-expr` | the executor | A window's `ORDER BY` on an expression rather than a column. |
| `refused-window-mixed` | the executor | A running aggregate beside a window over another partition or order. |
| `refused-lead-default` | the executor | `lead()` and `lag()` with a default value. |
