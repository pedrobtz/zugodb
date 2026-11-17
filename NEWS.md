# zuslothdb 0.0.0.9000

* `dbSendQuery()`, `dbFetch()`, `dbGetQuery()` and the other result
  methods return data frames. Boolean, integer, floating-point and
  `VARCHAR` columns arrive as logical, integer, double and UTF-8
  character; `dbConnect(bigint = )` chooses double, `integer64` or
  character for 64-bit integers. A value R cannot hold exactly is a
  `zuslothdb_type_error`, never a silent change.
* `dbExecute()` no longer reads a query's one-row `BIGINT` column named
  `Count` as a row count.
* `zuslothdb()` is a DBI driver, used as `duckdb::duckdb()` is: it holds
  one database, in memory or in a file, that its connections share.
  `dbConnect()`, `dbDisconnect()`, `dbIsValid()`, `dbGetInfo()` and
  `show()` work for the driver and the connection, and `dbExecute()`
  runs statements and returns the rows they changed.
* `dbConnect(read_only = TRUE)` refuses every statement that would change
  the database and never writes its file.
* `dbConnect(threads = )` caps the engine's threads, 2 by default; the
  engine has one thread pool per process, so the cap is process-wide.
* A file database is written when its last connection closes. A
  connection used in a forked child is a `zuslothdb_closed_error`.
* Errors are conditions inheriting from `zuslothdb_error`:
  `zuslothdb_sql_error` (the engine's message), `zuslothdb_type_error`,
  `zuslothdb_argument_error`, `zuslothdb_closed_error` and
  `zuslothdb_unsupported` (`params =`).
* The SlothDB engine is bundled and compiled with the package: upstream
  commit `8180f21` with seven patches, listed by `zuslothdb_info()`. The
  package is offline only: a remote path in a query is an error.
