# Connect to a zugodb database

Opens a connection to the driver's database. Two connections from the
same driver share one database; `dbConnect(zugodb(), dbdir = path)` is
the short form for a database of its own.

## Usage

``` r
# S4 method for class 'zugodb_driver'
dbConnect(
  drv,
  dbdir,
  read_only,
  bigint = "numeric",
  threads = 2L,
  timezone_out = "UTC",
  ...
)

# S4 method for class 'zugodb_connection'
dbIsValid(dbObj, ...)

# S4 method for class 'zugodb_connection'
show(object)

# S4 method for class 'zugodb_connection'
dbGetInfo(dbObj, ...)
```

## Arguments

- drv:

  A driver from
  [`zugodb()`](https://pedrobtz.github.io/zugodb/reference/zugodb.md).

- dbdir, read_only:

  As for
  [`zugodb()`](https://pedrobtz.github.io/zugodb/reference/zugodb.md).
  When either differs from the driver's, the connection gets a new
  driver, and so a database of its own.

- bigint:

  How 64-bit integers (`BIGINT`, `UINTEGER`, `UBIGINT`) arrive:
  `"numeric"` (the default, as in `duckdb`), a double, with a value
  beyond 2^53 a `zug_type_error`; `"integer64"`, the `bit64` class,
  which loads `bit64` (an error if it is not installed); or
  `"character"`, the decimal digits.

- threads:

  The number of threads the engine may use, a whole number from 1. The
  engine has one thread pool per R process, so this is a cap for the
  whole process, not for this connection: the last `dbConnect()` sets
  it, and
  [`zug_info()`](https://pedrobtz.github.io/zugodb/reference/zug_info.md)
  reports it.

- timezone_out:

  The time zone a `TIMESTAMP` column's `POSIXct` is shown in: `"UTC"`
  (the default, as in `duckdb`), another of
  [`OlsonNames()`](https://rdrr.io/r/base/timezones.html), or `""` for
  the session's. The instant is the same in any zone; an unknown zone is
  an error.

- ...:

  Not used; an unknown argument is an error.

- dbObj, object:

  A `zugodb_connection`.

## Value

An object of class `zugodb_connection`.

## Examples

``` r
con <- DBI::dbConnect(zugodb())
DBI::dbExecute(con, "CREATE TABLE t (x INTEGER)")
#> [1] 0
DBI::dbDisconnect(con)
```
