# zuslothdb 0.0.0.9000

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
