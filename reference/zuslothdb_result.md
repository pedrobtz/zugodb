# Query results

[`DBI::dbSendQuery()`](https://dbi.r-dbi.org/reference/dbSendQuery.html)
runs a query and returns a `zuslothdb_result`, from which
[`DBI::dbFetch()`](https://dbi.r-dbi.org/reference/dbFetch.html) returns
rows as a data frame;
[`DBI::dbGetQuery()`](https://dbi.r-dbi.org/reference/dbGetQuery.html)
does both at once. The whole result is computed when the query runs
(design D8); `dbFetch(res, n)` hands it over `n` rows at a time.

## Usage

``` r
# S4 method for class 'zuslothdb_connection,character'
dbSendQuery(conn, statement, ..., params = NULL, immediate = NULL)

# S4 method for class 'zuslothdb_connection,character'
dbSendStatement(conn, statement, ..., params = NULL, immediate = NULL)

# S4 method for class 'zuslothdb_result'
dbFetch(res, n = -1, ...)

# S4 method for class 'zuslothdb_result'
dbClearResult(res, ...)

# S4 method for class 'zuslothdb_result'
dbIsValid(dbObj, ...)

# S4 method for class 'zuslothdb_result'
dbHasCompleted(res, ...)

# S4 method for class 'zuslothdb_result'
dbGetRowCount(res, ...)

# S4 method for class 'zuslothdb_result'
dbGetRowsAffected(res, ...)

# S4 method for class 'zuslothdb_result'
dbGetStatement(res, ...)

# S4 method for class 'zuslothdb_result'
dbColumnInfo(res, ...)

# S4 method for class 'zuslothdb_result'
dbBind(res, params, ...)

# S4 method for class 'zuslothdb_result'
show(object)
```

## Arguments

- conn:

  A `zuslothdb_connection`.

- statement:

  A single SQL string.

- ...:

  Not used.

- params:

  Not supported: SlothDB has no prepared statements, so a non-`NULL`
  value is a `zuslothdb_unsupported` error.

- immediate:

  Ignored: every statement runs at once.

- res, dbObj, object:

  A `zuslothdb_result`.

- n:

  The number of rows to fetch: a whole number from 0, or `-1` or `Inf`
  for all that are left.

## Value

`dbSendQuery()` and `dbSendStatement()`: a `zuslothdb_result`.
`dbFetch()`: a data frame. The rest as in DBI.

## Details

Columns arrive as these R types:

|  |  |
|----|----|
| SlothDB | R |
| `BOOLEAN` | logical |
| `TINYINT`, `SMALLINT`, `INTEGER`, `UTINYINT`, `USMALLINT` | integer |
| `BIGINT`, `UINTEGER`, `UBIGINT` | double, `integer64` or character, by `bigint` |
| `FLOAT`, `DOUBLE` | double |
| `VARCHAR` | character, marked UTF-8 |

A value R cannot hold as asked is a `zuslothdb_type_error` naming the
column and the row, never a silent change: a 64-bit integer beyond 2^53
with `bigint = "numeric"`, an `INTEGER` of -2^31 (R's `NA`), or a string
that is not valid UTF-8 or holds a NUL. Other types arrive in a later
version; reading one is a `zuslothdb_type_error`.

A connection has one open result: sending another query clears the
first, with a warning.

## Examples

``` r
con <- DBI::dbConnect(zuslothdb())
res <- DBI::dbSendQuery(con, "SELECT 1 AS a, 'x' AS b")
DBI::dbFetch(res)
#>   a b
#> 1 1 x
DBI::dbClearResult(res)
DBI::dbGetQuery(con, "SELECT 42 AS answer")
#>   answer
#> 1     42
DBI::dbDisconnect(con)
```
