# Changelog

## zuslothdb 0.0.0.9000

- [`dbSendQuery()`](https://dbi.r-dbi.org/reference/dbSendQuery.html),
  [`dbFetch()`](https://dbi.r-dbi.org/reference/dbFetch.html),
  [`dbGetQuery()`](https://dbi.r-dbi.org/reference/dbGetQuery.html) and
  the other result methods return data frames. Boolean, integer,
  floating-point and `VARCHAR` columns arrive as logical, integer,
  double and UTF-8 character; `dbConnect(bigint = )` chooses double,
  `integer64` or character for 64-bit integers. A value R cannot hold
  exactly is a `zuslothdb_type_error`, never a silent change.
- [`dbExecute()`](https://dbi.r-dbi.org/reference/dbExecute.html) no
  longer reads a query’s one-row `BIGINT` column named `Count` as a row
  count.
- [`zuslothdb()`](https://pedrobtz.github.io/zuslothdb/reference/zuslothdb.md)
  is a DBI driver, used as `duckdb::duckdb()` is: it holds one database,
  in memory or in a file, that its connections share.
  [`dbConnect()`](https://dbi.r-dbi.org/reference/dbConnect.html),
  [`dbDisconnect()`](https://dbi.r-dbi.org/reference/dbDisconnect.html),
  [`dbIsValid()`](https://dbi.r-dbi.org/reference/dbIsValid.html),
  [`dbGetInfo()`](https://dbi.r-dbi.org/reference/dbGetInfo.html) and
  [`show()`](https://rdrr.io/r/methods/show.html) work for the driver
  and the connection, and
  [`dbExecute()`](https://dbi.r-dbi.org/reference/dbExecute.html) runs
  statements and returns the rows they changed.
- `dbConnect(read_only = TRUE)` refuses every statement that would
  change the database and never writes its file.
- `dbConnect(threads = )` caps the engine’s threads, 2 by default; the
  engine has one thread pool per process, so the cap is process-wide.
- A file database is written when its last connection closes. A
  connection used in a forked child is a `zuslothdb_closed_error`.
- Errors are conditions inheriting from `zuslothdb_error`:
  `zuslothdb_sql_error` (the engine’s message), `zuslothdb_type_error`,
  `zuslothdb_argument_error`, `zuslothdb_closed_error` and
  `zuslothdb_unsupported` (`params =`).
- The SlothDB engine is bundled and compiled with the package: upstream
  commit `8180f21` with seven patches, listed by
  [`zuslothdb_info()`](https://pedrobtz.github.io/zuslothdb/reference/zuslothdb_info.md).
  The package is offline only: a remote path in a query is an error.
