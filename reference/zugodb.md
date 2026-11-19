# The zugodb driver

A DBI driver for the bundled engine, libzugodb, derived from SlothDB.
The driver holds one database, in memory or in a file, which every
connection made from it shares, as `duckdb::duckdb()` does. The database
opens at the first
[`DBI::dbConnect()`](https://dbi.r-dbi.org/reference/dbConnect.html) and
closes with its last connection.

## Usage

``` r
zugodb(dbdir = ":memory:", read_only = FALSE)

# S4 method for class 'zugodb_driver'
show(object)

# S4 method for class 'zugodb_driver'
dbIsValid(dbObj, ...)

# S4 method for class 'zugodb_driver'
dbGetInfo(dbObj, ...)
```

## Arguments

- dbdir:

  `":memory:"` (the default) for an in-memory database, or the path of a
  database file. The file need not exist unless `read_only` is `TRUE`;
  its directory must.

- read_only:

  `TRUE` to refuse every statement that would change the database, and
  never write its file.

- object:

  A `zugodb_driver`.

- dbObj:

  A `zugodb_driver`.

- ...:

  Not used.

## Value

An object of class `zugodb_driver`, for
[`DBI::dbConnect()`](https://dbi.r-dbi.org/reference/dbConnect.html).

## Details

A file-backed database is read into memory when it opens and written
back, whole, when it closes: through
[`DBI::dbDisconnect()`](https://dbi.r-dbi.org/reference/dbDisconnect.html)
of its last connection, or at the end of the R session. The file is tied
to the engine commit that
[`zug_info()`](https://pedrobtz.github.io/zugodb/reference/zug_info.md)
reports.

## See also

[`DBI::dbConnect()`](https://dbi.r-dbi.org/reference/dbConnect.html),
whose zugodb method takes `threads`.

## Examples

``` r
drv <- zugodb()
con <- DBI::dbConnect(drv)
DBI::dbExecute(con, "CREATE TABLE t (x INTEGER)")
#> [1] 0
DBI::dbExecute(con, "INSERT INTO t VALUES (1), (2)")
#> [1] 2
DBI::dbDisconnect(con)
```
