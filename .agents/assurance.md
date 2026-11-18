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

- compiled: the vendored SlothDB C++20 engine and the glue over it.
- untrusted-input: the CSV, Parquet, JSON, Arrow IPC, Avro, Excel and
  SQLite readers and the database file parse bytes the package did not
  write.
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
| fuzz          | gap #21 |                    |
| fuzz-reach    | gap #21 |                    |

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
| the patched SlothDB engine              | upstream's own doctest suite (378 cases) on the vendored, patched tree | tools/upstream-tests.sh, in platforms.yaml |
| the vendored tree                       | upstream at the pinned commit plus tools/patches, rebuilt and compared file by file | tools/vendor/verify, in vendor.yaml |
| SQL results as R vectors (scalar types) | recorded `duckdb` answers for the same queries            | tests/testthat/test-oracle-scalar.R |
| SQL results as R vectors (`DATE`, `TIMESTAMP`, `BLOB`, and the refused types) | recorded `duckdb` answers for the same queries, by a table and by a set operation's boxed rows; each refused type a `zuslothdb_type_error` naming its column | tests/testthat/test-temporal.R, tests/testthat/test-oracle-types.R, tests/testthat/test-conditions.R |
| Parquet scan on decode workers: stopped before its last row group (#32), and their exit on Windows (#34) | every query returns within a time limit, in a child R, with the rows the `duckdb`-written fixture holds | tests/testthat/test-parquet-stop.R (macOS, Linux, Windows) |
| CSV, Parquet, JSON, Arrow IPC files     | recorded `duckdb` answers over `duckdb`-written fixtures  | gap #23    |
| Avro, Excel and SQLite files            | recorded answers from a reference reader                  | gap #23    |
| database file                           | round trip through a fresh R process                      | gap #23    |
| DBI specification                       | `DBItest`                                                 | gap #24    |
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
| connection | tests/testthat/test-lifecycle.R | tests/testthat/test-fork.R | tests/testthat/test-unwind.R | its database's external pointer, in `prot` (src/sdb_database.cpp) |
| result     | tests/testthat/test-lifecycle.R | n/a: materialised R-side buffers; its connection's guard refuses a foreign process | tests/testthat/test-unwind.R | its external pointer; a cleared or closed-connection result is refused (R/result.R) |
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
| an engine vector, 2048 rows, inside a table's row group (#45) | rows | tests/testthat/test-storage.R (NULLs at rows 2047 to 2049 and 4095 to 4097, by one VALUES list and by many inserts), oracle/scalar-nulls-long |
