# Connect to a zuslothdb database

Opens a connection to the driver's database. Two connections from the
same driver share one database; `dbConnect(zuslothdb(), dbdir = path)`
is the short form for a database of its own.

## Usage

``` r
# S4 method for class 'zuslothdb_driver'
dbConnect(drv, dbdir, read_only, threads = 2L, ...)

# S4 method for class 'zuslothdb_connection'
dbIsValid(dbObj, ...)

# S4 method for class 'zuslothdb_connection'
show(object)

# S4 method for class 'zuslothdb_connection'
dbGetInfo(dbObj, ...)
```

## Arguments

- drv:

  A driver from
  [`zuslothdb()`](https://pedrobtz.github.io/zuslothdb/reference/zuslothdb.md).

- dbdir, read_only:

  As for
  [`zuslothdb()`](https://pedrobtz.github.io/zuslothdb/reference/zuslothdb.md).
  When either differs from the driver's, the connection gets a new
  driver, and so a database of its own.

- threads:

  The number of threads the engine may use, a whole number from 1. The
  engine has one thread pool per R process, so this is a cap for the
  whole process, not for this connection: the last `dbConnect()` sets
  it, and
  [`zuslothdb_info()`](https://pedrobtz.github.io/zuslothdb/reference/zuslothdb_info.md)
  reports it.

- ...:

  Not used; an unknown argument is an error.

- dbObj, object:

  A `zuslothdb_connection`.

## Value

An object of class `zuslothdb_connection`.

## Examples

``` r
con <- DBI::dbConnect(zuslothdb())
DBI::dbExecute(con, "CREATE TABLE t (x INTEGER)")
#> [1] 0
DBI::dbDisconnect(con)
```
