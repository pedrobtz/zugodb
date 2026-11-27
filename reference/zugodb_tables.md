# Write, list and remove tables

[`DBI::dbWriteTable()`](https://dbi.r-dbi.org/reference/dbWriteTable.html)
creates a table from a data frame, or replaces one (`overwrite = TRUE`)
or adds to one (`append = TRUE`);
[`DBI::dbAppendTable()`](https://dbi.r-dbi.org/reference/dbAppendTable.html)
adds the rows of a data frame to a table that exists. Columns are
written as these SQL types, which
[`DBI::dbDataType()`](https://dbi.r-dbi.org/reference/dbDataType.html)
reports:

## Usage

``` r
# S4 method for class 'zugodb_connection,character'
dbWriteTable(
  conn,
  name,
  value,
  ...,
  row.names = FALSE,
  overwrite = FALSE,
  append = FALSE,
  field.types = NULL,
  temporary = FALSE
)

# S4 method for class 'zugodb_connection,Id'
dbWriteTable(
  conn,
  name,
  value,
  ...,
  row.names = FALSE,
  overwrite = FALSE,
  append = FALSE,
  field.types = NULL,
  temporary = FALSE
)

# S4 method for class 'zugodb_connection,SQL'
dbWriteTable(
  conn,
  name,
  value,
  ...,
  row.names = FALSE,
  overwrite = FALSE,
  append = FALSE,
  field.types = NULL,
  temporary = FALSE
)

# S4 method for class 'zugodb_connection,character'
dbAppendTable(conn, name, value, ..., row.names = NULL)

# S4 method for class 'zugodb_connection,Id'
dbAppendTable(conn, name, value, ..., row.names = NULL)

# S4 method for class 'zugodb_connection,SQL'
dbAppendTable(conn, name, value, ..., row.names = NULL)

# S4 method for class 'zugodb_connection'
dbListTables(conn, ...)

# S4 method for class 'zugodb_connection,character'
dbExistsTable(conn, name, ...)

# S4 method for class 'zugodb_connection,Id'
dbExistsTable(conn, name, ...)

# S4 method for class 'zugodb_connection,SQL'
dbExistsTable(conn, name, ...)

# S4 method for class 'zugodb_connection,character'
dbListFields(conn, name, ...)

# S4 method for class 'zugodb_connection,Id'
dbListFields(conn, name, ...)

# S4 method for class 'zugodb_connection,SQL'
dbListFields(conn, name, ...)

# S4 method for class 'zugodb_connection,character'
dbRemoveTable(conn, name, ..., temporary = FALSE, fail_if_missing = TRUE)

# S4 method for class 'zugodb_connection,Id'
dbRemoveTable(conn, name, ..., temporary = FALSE, fail_if_missing = TRUE)

# S4 method for class 'zugodb_connection,SQL'
dbRemoveTable(conn, name, ..., temporary = FALSE, fail_if_missing = TRUE)

# S4 method for class 'zugodb_connection,character'
dbQuoteIdentifier(conn, x, ...)

# S4 method for class 'zugodb_connection,character'
dbQuoteString(conn, x, ...)

# S4 method for class 'zugodb_connection,SQL'
dbQuoteString(conn, x, ...)

# S4 method for class 'zugodb_connection,SQL'
dbQuoteIdentifier(conn, x, ...)

# S4 method for class 'zugodb_connection'
dbQuoteLiteral(conn, x, ...)

# S4 method for class 'zugodb_connection,character'
dbReadTable(conn, name, ..., row.names = FALSE, check.names = TRUE)

# S4 method for class 'zugodb_connection,Id'
dbReadTable(conn, name, ..., row.names = FALSE, check.names = TRUE)

# S4 method for class 'zugodb_connection,SQL'
dbReadTable(conn, name, ..., row.names = FALSE, check.names = TRUE)

# S4 method for class 'zugodb_connection'
dbListObjects(conn, prefix = NULL, ...)

# S4 method for class 'zugodb_connection'
dbDataType(dbObj, obj, ...)

# S4 method for class 'zugodb_driver'
dbDataType(dbObj, obj, ...)

# S4 method for class 'zugodb_connection'
dbBegin(conn, ...)

# S4 method for class 'zugodb_connection'
dbCommit(conn, ...)

# S4 method for class 'zugodb_connection'
dbRollback(conn, ...)
```

## Arguments

- conn:

  A `zugodb_connection`.

- name:

  The table's name: a string,
  [`DBI::Id()`](https://dbi.r-dbi.org/reference/Id.html) with only a
  `table`, or [`DBI::SQL()`](https://dbi.r-dbi.org/reference/SQL.html)
  of a quoted name. Names keep their spelling and match ignoring ASCII
  case, as DuckDB's; a `"` in a name is doubled when it is quoted.

- value:

  A data frame.

- ...:

  Not used.

- row.names:

  As in
  [`DBI::sqlRownamesToColumn()`](https://dbi.r-dbi.org/reference/rownames.html):
  `FALSE` (the default) drops them, `TRUE` or a string writes them as a
  column.

- overwrite, append:

  `TRUE` to replace or to add to a table that exists; at most one of
  them.

- field.types:

  A named character vector of SQL types for some or all columns, in
  place of
  [`DBI::dbDataType()`](https://dbi.r-dbi.org/reference/dbDataType.html)'s.

- temporary:

  Not supported: the engine has no temporary tables.

- fail_if_missing:

  For `dbRemoveTable()`: `FALSE` to do nothing when the table does not
  exist.

- x:

  A character vector of identifiers.

- check.names:

  For `dbReadTable()`: `TRUE` (the default) to make the column names
  syntactic with
  [`make.names()`](https://rdrr.io/r/base/make.names.html).

- prefix:

  For `dbListObjects()`: `NULL`, since the engine has no schemas.

- dbObj:

  A `zugodb_driver` or `zugodb_connection`.

- obj:

  A vector, or a data frame for the type of each column.

## Value

`dbWriteTable()`, `dbRemoveTable()`: `TRUE`, invisibly.
`dbAppendTable()`: the number of rows written. `dbListTables()`,
`dbListFields()`: a character vector. `dbExistsTable()`: `TRUE` or
`FALSE`. `dbDataType()`: a character vector. `dbQuoteIdentifier()`,
`dbQuoteString()`:
[`DBI::SQL()`](https://dbi.r-dbi.org/reference/SQL.html).
`dbReadTable()`: a data frame. `dbListObjects()`: a data frame of
`table` ([`DBI::Id()`](https://dbi.r-dbi.org/reference/Id.html)s) and
`is_prefix`, always `FALSE`.

## Details

|  |  |
|----|----|
| R | Engine |
| logical | `BOOLEAN` |
| integer | `INTEGER` |
| double | `DOUBLE` |
| character, factor | `VARCHAR` |
| `Date` | `DATE` |
| `POSIXct` | `TIMESTAMP`, the instant in UTC, to the microsecond |
| [`bit64::integer64`](https://bit64.r-lib.org/reference/bit64-package.html) | `BIGINT` |
| [`blob::blob()`](https://blob.tidyverse.org/reference/blob.html), or a list of raw vectors | `BLOB` |

A column of another class, a value that does not fit its column (a
fraction for an `INTEGER`, a number out of range, a string that is not
valid UTF-8) or a column the table does not have is an error, raised
before any row is written: the table is as it was. `NA` is written as
`NULL`; `NaN` stays `NaN`.

The engine has no transactions:
[`DBI::dbBegin()`](https://dbi.r-dbi.org/reference/transactions.html),
[`DBI::dbCommit()`](https://dbi.r-dbi.org/reference/transactions.html)
and
[`DBI::dbRollback()`](https://dbi.r-dbi.org/reference/transactions.html)
are `zug_unsupported` errors, and SQL `BEGIN`, `COMMIT` and `ROLLBACK`
are refused.

## Examples

``` r
con <- DBI::dbConnect(zugodb())
DBI::dbWriteTable(con, "cars", head(mtcars))
DBI::dbListTables(con)
#> [1] "cars"
DBI::dbReadTable(con, "cars")
#>    mpg cyl disp  hp drat    wt  qsec vs am gear carb
#> 1 21.0   6  160 110 3.90 2.620 16.46  0  1    4    4
#> 2 21.0   6  160 110 3.90 2.875 17.02  0  1    4    4
#> 3 22.8   4  108  93 3.85 2.320 18.61  1  1    4    1
#> 4 21.4   6  258 110 3.08 3.215 19.44  1  0    3    1
#> 5 18.7   8  360 175 3.15 3.440 17.02  0  0    3    2
#> 6 18.1   6  225 105 2.76 3.460 20.22  1  0    3    1
DBI::dbAppendTable(con, "cars", tail(mtcars))
#> [1] 6
DBI::dbRemoveTable(con, "cars")
DBI::dbDisconnect(con)
```
