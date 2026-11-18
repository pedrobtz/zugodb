# Changelog

## zuslothdb 0.0.0.9000

- A Parquet column the engine would read wrongly is an error naming it,
  when a query uses it: `DECIMAL` (read unscaled), `TIME`, millisecond
  and nanosecond `TIMESTAMP` (read as raw counts), `UUID` and `INTERVAL`
  (read as `NULL`), and unsigned 32- and 64-bit integers (read as
  signed). Queries on the file’s other columns work
  ([\#36](https://github.com/pedrobtz/zuslothdb/issues/36),
  [\#37](https://github.com/pedrobtz/zuslothdb/issues/37),
  [\#38](https://github.com/pedrobtz/zuslothdb/issues/38)).
- [`dbWriteTable()`](https://dbi.r-dbi.org/reference/dbWriteTable.html),
  [`dbAppendTable()`](https://dbi.r-dbi.org/reference/dbAppendTable.html),
  [`dbListTables()`](https://dbi.r-dbi.org/reference/dbListTables.html),
  [`dbListFields()`](https://dbi.r-dbi.org/reference/dbListFields.html),
  [`dbExistsTable()`](https://dbi.r-dbi.org/reference/dbExistsTable.html),
  [`dbRemoveTable()`](https://dbi.r-dbi.org/reference/dbRemoveTable.html),
  [`dbDataType()`](https://dbi.r-dbi.org/reference/dbDataType.html) and
  [`dbQuoteIdentifier()`](https://dbi.r-dbi.org/reference/dbQuoteIdentifier.html)
  write and manage tables. Logical, integer, double, character, factor,
  `Date`, `POSIXct`, `integer64` and `blob` columns are written and read
  back identical; a column of another class, or a value its column
  cannot hold, is a `zuslothdb_type_error` before any row is written.
- SlothDB has no transactions, so
  [`dbBegin()`](https://dbi.r-dbi.org/reference/transactions.html),
  [`dbCommit()`](https://dbi.r-dbi.org/reference/transactions.html) and
  [`dbRollback()`](https://dbi.r-dbi.org/reference/transactions.html)
  are `zuslothdb_unsupported`, and SQL `ROLLBACK` is an error saying
  that nothing was undone.
- A table of more than 2048 rows with a `NULL` past row 2048 no longer
  reads back with `NULL`s where there are none, or crashes R: the engine
  kept a whole row group’s `NULL`s in a mask the size of one vector, and
  an `INSERT` of more than 2048 `VALUES` wrote past its buffers
  ([\#45](https://github.com/pedrobtz/zuslothdb/issues/45)).
- `DATE` columns arrive as `Date`, `TIMESTAMP` as `POSIXct` and `BLOB`
  as a list of raw vectors with `blob::blob()`’s classes.
  `dbConnect(timezone_out = )` sets the time zone a `POSIXct` is shown
  in, `"UTC"` by default as in `duckdb`; an unknown zone is an error.
- A `HUGEINT` or `TIME` column is a `zuslothdb_type_error`, since
  SlothDB computes their values wrongly (a `TIME` is always midnight),
  as are `INTERVAL` and `UUID`. SlothDB produces no `DECIMAL`, time
  zone, list, struct, map or union column.
- A set operation (`UNION`, `UNION ALL`, `INTERSECT`, `EXCEPT`) returns
  `TINYINT`, `SMALLINT` and `FLOAT` columns with their values; it
  returned numbers made of unwritten memory
  ([\#41](https://github.com/pedrobtz/zuslothdb/issues/41)).
- On Windows, a Parquet query on more than one thread that decodes
  several row groups no longer ends the R session
  ([\#34](https://github.com/pedrobtz/zuslothdb/issues/34)).
- A Parquet query that stops before the file’s last row group, such as a
  `LIMIT`, no longer hangs now and then: the engine’s decode workers
  could miss the signal to stop
  ([\#32](https://github.com/pedrobtz/zuslothdb/issues/32)).
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
- The SlothDB engine is bundled and compiled with the package, from
  pedrobtz/slothdb, zuslothdb’s fork of SlothDB: upstream commit
  `8180f21` and zuslothdb’s changes to it, listed by
  [`zuslothdb_info()`](https://pedrobtz.github.io/zuslothdb/reference/zuslothdb_info.md).
  The package is offline only: a remote path in a query is an error.
