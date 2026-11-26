# Assurance

How this package meets the shared `assurance.md` rules. CI checks this
file (r-actions `assurance.yml`), so keep the headings and table
columns as they are.

- **Statuses:** a cell is a fact, `gap #<issue>` or `n/a: <reason>`.

Written in Stage 0, before any engine code, with every row a tracked gap
whose issue says which stage of [roadmap.md](roadmap.md) fills it; Stage 1
backed the native checks and added the engine's own rows.

## Kinds

`compiled` `untrusted-input` `spec` `lifecycle` `emits`

- compiled: the vendored libzugodb C++20 engine and the glue over it.
- untrusted-input: the database file, which the engine parses whole on
  open; SQL reads no other file (libzugodb 0.2.0 refuses every route, and
  0.4.0 removed the readers), until Stage 10 links the zu family's.
- spec: the DBI specification, and SQL answers compared with DuckDB.
- lifecycle: database, connection and result objects behind external
  pointers.
- emits: database files, and data frames returned to R.

## Required checks

`Where` names the workflow file under `.github/workflows/` that runs the
check. Add `(own)` when that workflow is the package's own rather than a
call to r-actions.

| Check         | Status  | Where              |
| ------------- | ------- | ------------------ |
| sanitizers    | done    | native-checks.yaml |
| valgrind      | done    | native-checks.yaml |
| rchk          | done    | native-checks.yaml |
| gctorture     | done    | gctorture.yaml     |
| analyzers     | done    | native-checks.yaml |
| alloc-failure | done    | alloc-failure.yaml |
| mutation      | gap #20 |                    |
| fuzz          | n/a: the one parser of untrusted bytes, the database file's `Checkpoint::Load`, is libzugodb's and fuzzed in its CI at every pull request and weekly (`fuzz/checkpoint_load.cpp`, recomputing the CRC-32 so inputs reach the loader), at the tag vendored here; the glue parses no bytes (Stage 6, 2026-11-26) | |
| fuzz-reach    | n/a: as `fuzz`; libzugodb's `fuzz` job holds the reach | |

## Boundaries

One row per boundary the package crosses. An oracle knows the answer:

- a conformance suite or test vectors;
- a reference implementation;
- a round trip;
- equivalent inputs compared;
- the bytes the other side received.

"No crash" is not an oracle.

<!-- markdownlint-disable MD013 -->
| Boundary                                | Oracle                                                    | Carried by |
| --------------------------------------- | --------------------------------------------------------- | ---------- |
| the engine, libzugodb                   | libzugodb's own doctest suite (491 cases) on the vendored tree, with R's compilers and flags | tools/upstream-tests.sh, in platforms.yaml |
| the vendored tree                       | libzugodb at the pinned tag and commit, rebuilt and compared file by file | tools/vendor/verify, in vendor.yaml |
| SQL results as R vectors (scalar types) | recorded `duckdb` answers for the same queries            | tests/testthat/test-oracle-scalar.R |
| SQL results as R vectors (`DATE`, `TIMESTAMP`, `BLOB`, and the refused types) | recorded `duckdb` answers for the same queries, by a table and by a set operation's boxed rows; `TIME` a `zug_type_error` naming its column, `HUGEINT`, `INTERVAL` and `UUID` refused by the engine | tests/testthat/test-temporal.R, tests/testthat/test-oracle-types.R, tests/testthat/test-conditions.R |
| CSV, Parquet, JSON, Arrow IPC, Avro, Excel and SQLite files | refused, for a real file of each format, by every route: each table function, a path of each extension and a glob as a table, `DESCRIBE`, `COPY` to and from, `CREATE LIVE VIEW`, and a batch holding one (which runs none of its statements); a `COPY TO` writes no file | tests/testthat/test-refused.R |
| database file                           | round trip through a fresh R process, `identical()` to what was written; the golden file (tools/golden.R) against rows recorded from an in-memory database; another format version, a bit flipped at 360 offsets and more, truncated at each, extended, or not a database: each a `zug_sql_error` and the file unchanged | tests/testthat/test-persist.R, tests/testthat/test-hostile.R |
| the engine's parallel window sort       | its answer at threads 1 and 4, and R's own ranks; the exception rethrown after the join is libzugodb's doctest (`ArmWorkerFault`) | tests/testthat/test-conditions.R |
| files written                           | a listing of the working directory, `dbdir`'s directory and the home directory before and after a workload: only `dbdir` appears | tests/testthat/test-hygiene.R |
| Arrow C data and stream interfaces (`dbWriteTableArrow()`, `dbAppendTableArrow()`, `dbCreateTableArrow()`) | a second route to one value: each §6 type with NULLs through a `nanoarrow` stream is the table `dbWriteTable()` makes of the same data frame; round trip, `identical()` to the input, across a chunk and a row group, in uneven batches, for every string and binary layout, every timestamp unit and both date widths; each type the engine cannot hold refused from the schema, naming the column; a producer scripted to fail, or to give a batch of another layout: a classed error, nothing left by any route, and the producer's own count of its releases is 1 | tests/testthat/test-arrow-stream.R, tests/testthat/test-lifecycle.R |
| DBI specification                       | `DBItest`                                                 | gap #24    |
| data frames written into tables (`dbWriteTable()`, `dbAppendTable()`) | round trip: what is read back is `identical()` to what went in, for every class written, with `NA`, 0 rows and across 2048-row chunks, and under a hostile locale; SQL `CAST` of what the engine stored, for `integer64` | tests/testthat/test-roundtrip.R, tests/testthat/test-write.R, tests/testthat/test-identifiers.R |
<!-- markdownlint-enable MD013 -->

## Objects

One row per object that outlives the call that made it. "Sequences" is
the test of its out-of-order use (abort then read, use after end, use
after free). The guard columns name the test, or say why the guard
cannot apply.

<!-- markdownlint-disable MD013 -->
| Object     | Sequences | Fork    | Unwind  | Owner   |
| ---------- | --------- | ------- | ------- | ------- |
| database   | tests/testthat/test-lifecycle.R | tests/testthat/test-fork.R | tests/testthat/test-unwind.R | the driver (R/driver.R); connections keep it alive through `prot` |
| connection | tests/testthat/test-lifecycle.R | tests/testthat/test-fork.R | tests/testthat/test-unwind.R | its database's external pointer, in `prot` (src/zug_database.cpp) |
| result     | tests/testthat/test-lifecycle.R | n/a: materialised R-side buffers; its connection's guard refuses a foreign process | tests/testthat/test-unwind.R | its external pointer; a cleared or closed-connection result is refused (R/result.R) |
| Arrow C stream, in a load | tests/testthat/test-lifecycle.R (released by its producer, read twice, a batch of another layout, a closed connection, an error after the take) | n/a: held only for the call that loads it; the holder records its process and releases nothing in another (src/zug_stream.cpp) | tests/testthat/test-lifecycle.R (an error after the take releases it once, by the scripted producer's count); R/arrow.R's `on.exit()`, the finalizer last | the holder (src/zug_stream.cpp), from the move out of the producer's object to the end of the load |
<!-- markdownlint-enable MD013 -->

## Limits

One row per limit the package enforces. "Straddled by" is the test of
`limit - 1`, `limit` and `limit + 1` on every path that reaches it.

| Limit                         | Unit          | Straddled by |
| ----------------------------- | ------------- | ------------ |
| 2^53, the largest exact double | integer value | tests/testthat/test-bigint.R |
| -2^31, R's integer NA         | integer value | tests/testthat/test-spike.R |
| R's integer range, 2^31 - 1 and -(2^31 - 1) | integer value | tests/testthat/test-spike.R |
| `threads`, 1 to 2^31 - 1                     | threads       | tests/testthat/test-connect.R |
| an append chunk, 2048 rows (`src/zug_write.cpp`) | rows | tests/testthat/test-roundtrip.R (2048, 2049 and 5000 rows) |
| `SMALLINT`, `INTEGER` and `BIGINT` ranges on write | integer value | tests/testthat/test-write.R (-32768, 32767 and 32768; -2^63, 2^53 and 2^63) |
| an engine vector, 2048 rows, inside a table's row group (#45) | rows | tests/testthat/test-storage.R (NULLs at rows 2047 to 2049 and 4095 to 4097, by one VALUES list and by many inserts), oracle/scalar-nulls-long |
| an Arrow batch against a chunk, 2048 rows, and a row group, 122,880 (`src/zug_stream.cpp`) | rows | tests/testthat/test-arrow-stream.R (2047 to 2049 and 122,879 to 122,881 rows, NULLs on each side; batches of 2047, 2049, 1 and 2048) |
| uint64 into `BIGINT`, 2^63 - 1 | integer value | tests/testthat/test-arrow-stream.R (2^63 - 2, 2^63 - 1 and 2^63) |
| `TINYINT`, `SMALLINT` and `INTEGER` ranges, and 2^53 for a `DOUBLE`, on an append from a stream | integer value | tests/testthat/test-arrow-stream.R (each bound and one past it, both signs) |
| a nanosecond timestamp, whole microseconds only | nanoseconds | tests/testthat/test-arrow-stream.R (999, 1000 and 1001) |
