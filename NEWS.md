# zugodb 0.0.0.9000

* What the engine's SQL gets right is measured (design Q18). On DuckDB
  v2.0.0's sqllogictest subset, libzugodb 0.4.0 gives no wrong answer
  and no crash or hang; most of what it cannot do is refused with an
  error. Through zugodb, joins, `GROUP BY`, window functions, subqueries,
  CTEs, set operations and `ORDER BY` with `LIMIT` agree with `duckdb`,
  on one thread and on two. The queries the engine refuses (correlated
  subqueries, window frames, ...) and the one listed difference, a
  column without an alias named `expr`, are listed in
  `tests/testthat/oracle/known-differences.md`.

* `dbWriteTableArrow()`, `dbAppendTableArrow()` and `dbCreateTableArrow()`
  take an Arrow C stream from any producer (a stream handle from
  zuarrow, nanoarrow, arrow or an ADBC driver, or a data frame with
  `zuarrow` installed) and load it into a table
  batch by batch, with no data frame in between. The stream is read once
  and released. A column of a type the engine cannot hold (a decimal, a
  time, a list, a struct, a dictionary, ...) is a `zug_type_error` naming
  it, before any row is read; a producer's error is a new
  `zug_stream_error`. A load that fails part-way leaves nothing: an
  append or an overwrite goes through a staging table first.

* The engine is libzugodb 0.4.0 (it was 0.1.1). What changes for users:
  - SQL reads and writes no file: every file table function
    (`read_csv()`, `read_parquet()`, `read_json()`, ...), a file path used
    as a table, `DESCRIBE` of one, `COPY` to or from a file and `CREATE
    LIVE VIEW` are a `zug_sql_error`, "Reading or writing files from SQL
    is not supported". Files come back through the zu family's readers
    (design §7).
  - A database file is format 1001, with a checksum. A file written by
    earlier versions of zugodb (format 1), a damaged one, a truncated one
    and one that is not a database are each a `zug_sql_error` when
    opened, and are left as they were.
  - DuckDB's answers where SlothDB's differed: names match ignoring
    case, `TINYINT` arithmetic overflows as an error, `substr()` counts
    characters, a `BLOB` literal's `\x` escapes are bytes, `Infinity` fits
    a `FLOAT` column, a `NaN` written is stored and read back as `NaN`,
    and many wrong answers are fixed (libzugodb's `CHANGELOG.md`).
  - `BEGIN`, `COMMIT` and `ROLLBACK` are refused, and `HUGEINT`,
    `INTERVAL`, `UUID` and `DECIMAL` are refused by SQL. `TIME` values
    are right now, and still a `zug_type_error` when read (#43).
  - `dbExecute()` of `CREATE TABLE ... AS` returns the rows it made.
  - The package bundles no zstd, snappy, fastpforlib or miniz.

* The package is renamed from zuslothdb to zugodb, and its repository to
  pedrobtz/zugodb (design D12). The driver is `zugodb()`, `zuslothdb_info()`
  is `zug_info()`, the classes are `zugodb_driver`, `zugodb_connection`
  and `zugodb_result`, the conditions are `zug_error`, `zug_sql_error`,
  `zug_type_error`, `zug_unsupported`, `zug_closed_error` and
  `zug_argument_error`, and the test switches are `ZUG_SKIP_HEAVY`,
  `ZUG_SLOW_TESTS` and `ZUG_TEST_SEED`. Nothing else changes.
* The engine is now libzugodb (<https://github.com/pedrobtz/libzugodb>),
  this package's own fork of SlothDB, vendored by release tag. Its 0.1.1
  fixes a data race on every multi-threaded Parquet scan, which UBSan under
  libstdc++ 15 stopped as "unreachable program point".
  `zug_info()` reports its tag and the SlothDB commit it descends
  from (`origin`, `origin_commit`), and no longer a list of changes.
  Nothing else changes.
* SQLite, Excel and Avro files are not supported: SlothDB's readers for
  the first two returned wrong values against real files, and its Avro
  reader is untested. `sqlite_scan()`, `read_xlsx()`, `read_avro()` and
  their file extensions are an error saying so (#55).
* Arrow IPC files are not supported: SlothDB's Arrow format is its own,
  and reads no file another Arrow tool writes. `read_arrow()` and a
  `.arrow`, `.feather` or `.ipc` path are an error saying so (#40).
* A JSON file the engine would read wrongly is an error naming the record
  and the key: a nested object or array, a key the first record does not
  have, or a value whose type is not the first record's for that key.
  Before, such values were dropped or coerced (#39). `\u` escapes are
  decoded.
* A Parquet column the engine would read wrongly is an error naming it,
  when a query uses it: `DECIMAL` (read unscaled), `TIME`, millisecond
  and nanosecond `TIMESTAMP` (read as raw counts), `UUID` and `INTERVAL`
  (read as `NULL`), and unsigned 32- and 64-bit integers (read as
  signed). Queries on the file's other columns work (#36, #37, #38).
* `dbWriteTable()`, `dbAppendTable()`, `dbListTables()`,
  `dbListFields()`, `dbExistsTable()`, `dbRemoveTable()`,
  `dbDataType()` and `dbQuoteIdentifier()` write and manage tables.
  Logical, integer, double, character, factor, `Date`, `POSIXct`,
  `integer64` and `blob` columns are written and read back identical; a
  column of another class, or a value its column cannot hold, is a
  `zug_type_error` before any row is written.
* SlothDB has no transactions, so `dbBegin()`, `dbCommit()` and
  `dbRollback()` are `zug_unsupported`, and SQL `ROLLBACK` is an
  error saying that nothing was undone.
* A table of more than 2048 rows with a `NULL` past row 2048 no longer
  reads back with `NULL`s where there are none, or crashes R: the engine
  kept a whole row group's `NULL`s in a mask the size of one vector, and
  an `INSERT` of more than 2048 `VALUES` wrote past its buffers (#45).
* `DATE` columns arrive as `Date`, `TIMESTAMP` as `POSIXct` and `BLOB` as
  a list of raw vectors with `blob::blob()`'s classes.
  `dbConnect(timezone_out = )` sets the time zone a `POSIXct` is shown
  in, `"UTC"` by default as in `duckdb`; an unknown zone is an error.
* A `HUGEINT` or `TIME` column is a `zug_type_error`, since SlothDB
  computes their values wrongly (a `TIME` is always midnight), as are
  `INTERVAL` and `UUID`. SlothDB produces no `DECIMAL`, time zone,
  list, struct, map or union column.
* A set operation (`UNION`, `UNION ALL`, `INTERSECT`, `EXCEPT`) returns
  `TINYINT`, `SMALLINT` and `FLOAT` columns with their values; it returned
  numbers made of unwritten memory (#41).
* On Windows, a Parquet query on more than one thread that decodes
  several row groups no longer ends the R session (#34).
* A Parquet query that stops before the file's last row group, such as a
  `LIMIT`, no longer hangs now and then: the engine's decode workers
  could miss the signal to stop (#32).
* `dbSendQuery()`, `dbFetch()`, `dbGetQuery()` and the other result
  methods return data frames. Boolean, integer, floating-point and
  `VARCHAR` columns arrive as logical, integer, double and UTF-8
  character; `dbConnect(bigint = )` chooses double, `integer64` or
  character for 64-bit integers. A value R cannot hold exactly is a
  `zug_type_error`, never a silent change.
* `dbExecute()` no longer reads a query's one-row `BIGINT` column named
  `Count` as a row count.
* `zugodb()` is a DBI driver, used as `duckdb::duckdb()` is: it holds
  one database, in memory or in a file, that its connections share.
  `dbConnect()`, `dbDisconnect()`, `dbIsValid()`, `dbGetInfo()` and
  `show()` work for the driver and the connection, and `dbExecute()`
  runs statements and returns the rows they changed.
* `dbConnect(read_only = TRUE)` refuses every statement that would change
  the database and never writes its file.
* `dbConnect(threads = )` caps the engine's threads, 2 by default; the
  engine has one thread pool per process, so the cap is process-wide.
* A file database is written when its last connection closes. A
  connection used in a forked child is a `zug_closed_error`.
* Errors are conditions inheriting from `zug_error`:
  `zug_sql_error` (the engine's message), `zug_type_error`,
  `zug_argument_error`, `zug_closed_error` and
  `zug_unsupported` (`params =`).
* The SlothDB engine is bundled and compiled with the package, from
  pedrobtz/slothdb, zugodb's fork of SlothDB: upstream commit
  `8180f21` and zugodb's changes to it, listed by `zug_info()`. The
  package is offline only: a remote path in a query is an error.
