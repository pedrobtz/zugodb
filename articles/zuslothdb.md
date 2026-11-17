# Get started with zuslothdb

zuslothdb runs SQL over local files from R. It compiles the experimental
[SlothDB](https://github.com/SouravRoy-ETL/slothdb) engine into the
package and puts a DBI driver on top, so there is no server, no system
library and no download. This page connects, queries one CSV file, and
shows how the package handles types, failures and writing.

``` r

library(DBI)
library(zuslothdb)

cars <- tempfile(fileext = ".csv")
write.csv(mtcars, cars, row.names = FALSE)
```

## Connect and query a file

[`zuslothdb()`](https://pedrobtz.github.io/zuslothdb/reference/zuslothdb-package.md)
is a DBI driver, used the way `duckdb::duckdb()` is: code moves between
the two by changing the first argument of `dbConnect()`. The engine
reads files through SQL table functions, so there is no import step.

``` r

con <- dbConnect(zuslothdb(), dbdir = ":memory:")

dbGetQuery(con, paste0(
  "SELECT cyl, count(*) AS n, avg(mpg) AS mpg ",
  "FROM read_csv(", dbQuoteString(con, cars), ") GROUP BY cyl ORDER BY cyl"
))

# Open: what zuslothdb_read() returns and whether it takes reader options (Q12)
zuslothdb_read(con, cars)
```

## Values arrive as R types, or not at all

Every engine type maps to one R type, and a value that would lose
information is an error instead of a rounded number. A 64-bit integer is
a double by default; beyond 2^53 you choose a wider type when
connecting.

``` r

dbGetQuery(con, "SELECT DATE '2026-11-17' AS day, 42::BIGINT AS n")

dbGetQuery(con, "SELECT 9007199254740993::BIGINT AS n")
# a zuslothdb_type_error

con64 <- dbConnect(zuslothdb(), bigint = "integer64")
dbGetQuery(con64, "SELECT 9007199254740993::BIGINT AS n")
dbDisconnect(con64)
```

## Failures are classed conditions

The engine never takes the R session down: its errors come back as
conditions that inherit from `zuslothdb_error`, so a caller can tell a
bad query from a missing feature. SlothDB has no prepared statements, so
parameters are refused in v0.1.0.

``` r

tryCatch(
  dbGetQuery(con, "SELECT no_such_column FROM read_csv('missing.csv')"),
  zuslothdb_sql_error = function(e) conditionMessage(e)
)

dbGetQuery(con, "SELECT ? AS x", params = list(1))
# a zuslothdb_unsupported error
```

## Write tables and keep them

Data frames are written through the engine’s appender, and a file
database persists them. A database file is tied to the package version
that wrote it.

``` r

dbWriteTable(con, "cars", mtcars)
dbAppendTable(con, "cars", mtcars[1:3, ])

dbBegin(con)
dbExecute(con, "DELETE FROM cars WHERE cyl = 8")
dbRollback(con)

dbListTables(con)
dbDisconnect(con, shutdown = TRUE)
```

## Where next

The [reference index](https://pedrobtz.github.io/zuslothdb/reference/)
lists every function.
[`zuslothdb_info()`](https://pedrobtz.github.io/zuslothdb/reference/zuslothdb_info.md)
reports the engine commit, the patches applied and the thread count.
