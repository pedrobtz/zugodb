# RFC 0075: `zuslothdb`, a DBI package that vendors the SlothDB SQL engine

Adopted from [RFC 0075](https://github.com/pedrobtz/packages/blob/03a1c720b7731dbabdd77ae8688d1ebdbc84baac/rfcs/not-started/0075-slothdb-embedded-sql-engine-for-r.md)
on 2026-11-17. This file is the single source of truth for the design; the
RFC is now a pointer here. Changed since adoption: §5's `dbConnect()`
signature and method list, the new §5.1, Q9 to Q13 in §14, the
recorded-answer oracle and fuzzing in §10, §11, whose stages moved
to [roadmap.md](roadmap.md), and the package's name (Q8, `zuslothdb`).

- **Status:** Proposed. Merging this file adopts the design as the starting
  point for the package's own `.agents/design.md`; each decision in
  section 12 stays the maintainer's until the package repository records
  it. Stage 1 (§11) is a go/no-go spike: upstream has been dormant since
  2026-06-03 with red CI (§1.3), so D1, which admits the package at all,
  is the one most worth a second look.
- **Date:** 2026-11-17.
- **Packages:** `zuslothdb`, renamed from the RFC's `slothdb` on
  2026-11-17 (Q8); the repository moved from `pedrobtz/slothdb` to
  `pedrobtz/zuslothdb`. Neither name is on CRAN
  (`tools::CRAN_package_db()`, 2026-11-17); the RFC found crandb
  returning `not_found` for `slothdb` and `rslothdb`, and no CRAN
  package name containing `sloth`. `Imports: DBI (>= 1.2.0), methods`. `Suggests:
  testthat (>= 3.2.0), withr, DBItest, bit64, nanoarrow`. Vendors SlothDB
  v0.2.7+ at commit `8180f21` (MIT, "SlothDB Contributors"), which
  itself carries zstd's decompressor (BSD-3-Clause, of the BSD/GPL-2.0
  dual licence), snappy (BSD-3-Clause), fastpforlib (Apache-2.0, copied
  from DuckDB), miniz 3.0.0 (Unlicense) and `unordered_dense.h` (MIT).
  MIT for the package's own code.
- **Source:** a clone of
  [SouravRoy-ETL/slothdb](https://github.com/SouravRoy-ETL/slothdb) at
  `main`, commit `8180f215847015944c935f56f008d194e2c1494c`
  (2026-06-03), read but not built: `README.md`, `LICENSE`,
  `CMakeLists.txt`, `src/CMakeLists.txt`, `include/slothdb/api/slothdb.h`,
  `src/api/slothdb_api.cpp`, `include/slothdb/main/connection.hpp`,
  `include/slothdb/common/types/logical_type.hpp`, the `third_party/`
  tree, `python/slothdb`, `packaging/` and `docs/`; the repository's
  Actions runs and metadata through `gh`; the
  [duckdb-r](https://github.com/duckdb/duckdb-r) `src/` tree and
  crandb's `duckdb` 2.0.0 record as the model; `tools::CRAN_package_db()`
  (25,792 packages). Nothing has been built.

## 1. What zuslothdb is

`zuslothdb` is to SlothDB what `duckdb` is to DuckDB: the engine's source
vendored into an R package, compiled at install, with a DBI driver on
top, so that

```r
con <- DBI::dbConnect(zuslothdb::zuslothdb(), dbdir = ":memory:")
DBI::dbGetQuery(con, "SELECT count(*) FROM 'flights.parquet'")
```

works with no system library, no server and no download.

SlothDB describes itself as "an experimental embedded SQL engine in
C++20" and "a learning exercise; expect rough edges". It is a
from-scratch columnar engine whose layout follows DuckDB's (api, binder,
catalog, execution, optimizer, parser, planner, storage, transaction),
with built-in readers for CSV, Parquet, JSON, Avro, Excel, Arrow IPC and
SQLite files and its own single-file storage format.

> **SQL over local files from R, with nothing to install but the
> package, and the engine never takes R down.**

### 1.1 What zuslothdb is not

- Not a DuckDB replacement. `duckdb` 2.0.0 is on CRAN and is the
  mature choice; §1.2 is the honest comparison.
- Not a network client. SlothDB's HTTP and S3 readers, its extension
  loader and its natural-language `ask` module are compiled out (§4.2).
- Not a `dplyr` backend of its own in v0.1.0; `dbplyr` works through
  DBI to the extent SlothDB's SQL does (Q5).

### 1.2 Where it sits among CRAN packages

Checked on 2026-11-17.

- **`duckdb`** 2.0.0 (2026-10-23): `Depends: DBI, R (>= 4.2.0)`,
  `Imports: methods, utils`, no `LinkingTo`. It vendors DuckDB under
  `src/duckdb/` as about 150 unity-build `ub_*.cpp` files, with glue in
  `src/*.cpp` over the C++ API through a vendored `cpp11.cpp`: DBI
  methods, Arrow export, ALTREP data frames over relations
  (`reltoaltrep.cpp`), and R functions registered as SQL. Its tarball
  is 8,307,869 bytes.
- **`duckplyr`** 1.2.2, **`dbplyr`** 2.6.0 and **`adbcdrivermanager`**
  0.25.0 consume engines; **`RSQLite`** 3.53.3 is the row-store
  counterpart.

What `zuslothdb` can claim against `duckdb`, to be measured in Stage 1
rather than asserted: a source tree about a tenth of the size
(SlothDB's `src/` is 2.5 MiB, `include/` 0.5 MiB, `third_party/`
1.8 MiB including the unused llama.cpp submodule stub), so a shorter
install from source; and its own Parquet and CSV code paths as a second
implementation to test R data pipelines against. Its README's ClickBench
wins over DuckDB are called "anecdotal" by the README itself, and some
come from query-shape-specific handlers, so speed is not the pitch.

### 1.3 Upstream health

The facts that decide D1:

- **Activity.** Created 2026-04-15; 573 commits, 572 of them by the
  principal author; 233 in April, 338 in May, 2 in June, none since
  2026-06-03. Releases v0.1.5 to v0.2.7 (2026-05-20). 412 stars.
- **CI.** Every non-CodeQL workflow run from 2026-05-22 to `8180f21`
  failed, at the test step on Linux and Windows. The README still says
  green on all three. Windows CI is MSVC only.
- **Conformance.** Recent commit messages put sqllogictest parity near
  18%; `bench/correctness/sqllogictest/` fetches the corpus. 408 doctest
  cases under `test/unit`.
- **Shape.** The first commit added 32,968 lines;
  `physical_planner.cpp` is 14,573 lines. The commit cadence (76 in one
  day) and metric-suffixed messages suggest heavy machine assistance;
  that is an inference, and it matters only because it means few
  humans know the code.
- **ABI.** No stability promise; the C API (§4.1) is versioned only by
  `slothdb_version()`.

Vendoring a dormant upstream means `zuslothdb` maintains a fork. §4.2
lists the patches that fork starts with.

### 1.4 The admission test

Against [base-layer-candidates.md §2][admission], which this package,
outside the `zu*` family, does not strictly need to pass but should be
honest about: (1) there is no conformance corpus SlothDB passes, though
sqllogictest and DuckDB itself are oracles to test against, so it
passes as a test plan only; (2) MIT C++20 with permissively licensed
dependencies and no system library passes, at the price of §4.2;
(3) no sibling needs it: `quak` and `zusql` (RFC 0066) target DuckDB SQL,
and `zuarrow` (RFC 0041) is a consumer of anything with Arrow; (4) R
already has `duckdb`, so this fails. D1 records why to build it anyway,
or not.

## 2. Scope

In v0.1.0: in-memory and file databases; `dbGetQuery`, `dbExecute`,
`dbSendQuery` with chunked fetch; `dbWriteTable` and `dbAppendTable`
through SlothDB's C++ `Append`; `dbListTables`, `dbListFields`,
`dbExistsTable`, `dbRemoveTable`; transactions; R types for every
SlothDB logical type that has one (§6); reading CSV, Parquet, JSON and
Arrow IPC files through SQL; `zuslothdb_info()`. Later: parameter
binding (§5, Q3), Arrow C data interface export through `nanoarrow`,
ALTREP results, R functions as SQL functions, `dbplyr` translations.

## 3. Position in the portfolio

- **`zusql`** (RFC 0066) parses PostgreSQL SQL; SlothDB has its own
  parser, so the two share nothing but tests: queries `zusql` reports as
  reading files are queries `zuslothdb` can run.
- **`zuarrow`** (RFC 0041) and **`nanoarrow`**: SlothDB has no Arrow C
  data interface (§4.1). Export is later work in the glue, not upstream.
- **`quak`** builds DuckDB SQL over ADLS; it stays on `duckdb`, whose
  `delta_scan()` and Azure support SlothDB lacks.

Family conventions that still apply: `Language: en-GB`,
`Config/roxygen2/version: 8.1.0`; hidden symbols, exporting only
`R_init_zuslothdb`, which SlothDB's `duckdb_fastpforlib` namespace makes
necessary if `duckdb` is loaded in the same session; `zuslothdb_error`
conditions; `ZUSLOTHDB_SKIP_HEAVY` and `ZUSLOTHDB_SLOW_TESTS`; CI through
`pedrobtz/r-actions`.

## 4. Architecture

Three layers, as in `duckdb`:

1. **Vendored C++**: `src/`, `include/` and the compiled subset of
   `third_party/` (zstd decompression with `ZSTD_DISABLE_ASM`, snappy,
   fastpforlib, the bitpacking files, miniz), copied by
   `tools/vendor.R` from a pinned commit with §4.2's patches applied.
   No CMake at install: `vendor.R` writes the source list into
   `src/Makevars` and `Makevars.win`, since upstream collects sources
   with `file(GLOB_RECURSE)`. `CXX_STD = CXX20`.
2. **Glue C++**, prefix `sdb_`, over SlothDB's C++ API (D2): database
   and connection objects in external pointers with finalizers; query
   results converted column by column from `DataChunk`s to R vectors.
   Every call into SlothDB is wrapped in `try`/`catch`; no exception
   crosses into R, and no R API call that can longjmp runs while a
   SlothDB object with a destructor is live on the stack (§4.3).
3. **R**: the DBI classes (`zuslothdb_driver`, `zuslothdb_connection`,
   `zuslothdb_result`), argument checks and conditions.

### 4.1 Which API to bind

SlothDB's C API, `include/slothdb/api/slothdb.h` (183 lines), has
`slothdb_open`, `_connect`, `_query`, `_result_error`, per-cell
accessors, whole-column buffers for `int32`, `int64`, `double`,
validity and `varchar`, catalog introspection and `slothdb_version`. Its
type enum has six members (`BOOLEAN`, `INTEGER`, `BIGINT`, `FLOAT`,
`DOUBLE`, `VARCHAR`); `slothdb_api.cpp` returns dates and timestamps as
ISO strings and `SLOTHDB_TYPE_INVALID` for decimals, blobs, small and
unsigned integers, lists and structs. It has no prepared statements, no
appender, no streaming and no Arrow. The Python binding uses it through
`ctypes` and inherits those limits.

The C++ API (`include/slothdb/main/connection.hpp`) has `Query`,
`Append(table, DataChunk)` and `Begin`, `Commit`, `Rollback`, and
`LogicalTypeId` closely mirrors DuckDB's, with `DECIMAL`, `BLOB`,
`LIST`, `MAP`, `STRUCT` and `UNION`. `duckdb` binds DuckDB's C++ API for
the same reasons. D2 takes the C++ API; the price is that every
upstream refactor is a glue change, which a pinned, patched fork makes
a choice rather than a surprise.

### 4.2 Patches

Each is recorded in `tools/vendor.yaml` with its reason. If they
together pass a few thousand lines, or Stage 1 finds the engine wrong
on its own tests, D1's stop condition applies.

- **MinGW.** Windows paths in `http_client.cpp`,
  `extension_manager.cpp`, `file_handle.cpp`, `fast_csv_reader.cpp`,
  `hive_partition.cpp` and `database.cpp` are gated on `_MSC_VER`, so
  Rtools' GCC takes the POSIX branch (`dlfcn.h`, `mmap`,
  `sys/socket.h`). Gate on `_WIN32`; drop `#pragma comment(lib, ...)`
  and MSVC `#pragma warning`.
- **Baseline ISA.** `src/CMakeLists.txt` compiles the whole library
  with `-mavx2` on every x86 build and snappy with `-mavx2 -mbmi2`;
  `src/` has 66 `_mm*` intrinsic uses. CRAN binaries must run on
  baseline x86-64. Remove the flags and either guard the AVX2 kernels
  with `__attribute__((target("avx2")))` behind a CPUID check, or drop
  them to their scalar paths (Q2).
- **Process output.** Ten `fprintf(stderr, ...)` calls in `src/`, most
  behind `SLOTH_PROFILE`: remove. `abort()` appears only in
  `unordered_dense.h`; map it to a thrown exception.
- **Network, extensions, model downloads.** Compile out the HTTP client
  (HTTPS works only through WinHTTP anyway), S3, `dlopen` extension
  loading, and the whole `src/ask/` module, which downloads a model to
  `~/.slothdb/models` at run time. CRAN forbids writing to the home
  directory and downloading code; offline is also the honest scope.
- **Threads.** `thread_pool.cpp` and `dict_histogram.cpp` size pools
  from `std::thread::hardware_concurrency()`, overridden only by
  `SLOTHDB_MAX_THREADS`. Default to two threads unless the user sets
  `threads` in `dbConnect()`, matching CRAN's two-core check limit.
- **Memory.** `DatabaseConfig::memory_limit` is declared and never
  read, so there is no limit to expose; `dbConnect(memory_limit = )`
  is absent until it exists (Q4).

### 4.3 Exceptions, interrupts and lifetimes

SlothDB throws C++ exceptions; R errors longjmp. The glue converts in
one direction only: C++ exceptions become a returned error string
raised as `zuslothdb_error` after every C++ frame has unwound, the
pattern `cpp11::unwind_protect` encodes. Long queries check
`R_CheckUserInterrupt` from the R thread only, through a cancellation
flag SlothDB's executor must poll; whether it can is Stage 1's
question (Q6). Closing a connection with live results invalidates them,
as in `duckdb`; a garbage-collected database closes on finalisation
with a warning.

## 5. Public R API (proposed)

```r
drv <- zuslothdb()                          # DBIDriver
con <- dbConnect(drv, dbdir = ":memory:", read_only = FALSE,
                 threads = 2L, bigint = "numeric",
                 decimal = "numeric", timezone_out = "UTC")
dbGetQuery(con, "SELECT * FROM read_csv('x.csv')")
res <- dbSendQuery(con, sql); dbFetch(res, n = 1e5); dbClearResult(res)
dbWriteTable(con, "t", df); dbAppendTable(con, "t", df)
dbListTables(con); dbListFields(con, "t"); dbRemoveTable(con, "t")
dbBegin(con); dbCommit(con); dbRollback(con)
dbDisconnect(con, shutdown = TRUE)
zuslothdb_info()        # engine commit, version string, patches, threads
zuslothdb_read(con, path, format = NULL)  # thin helper over SQL readers
```

The remaining DBI generics that `DBItest` (§10) exercises are
implemented without being listed above: `dbIsValid`, `dbGetInfo`,
`dbDataType`, `dbQuoteIdentifier`, `dbColumnInfo`, `dbHasCompleted`,
`dbGetRowCount`, `dbGetRowsAffected`, `dbGetStatement`, `dbExistsTable`
and `show`. `bigint`, `decimal` and `timezone_out` are §6's options;
`bigint` also takes `"integer64"` and `"character"`, `decimal` also
takes `"character"`.

`dbGetQuery(params = )` and `dbBind()` raise `zuslothdb_unsupported` in
v0.1.0, since SlothDB has no prepared statements (Q3). Conditions
inherit from `zuslothdb_error`: `zuslothdb_sql_error` (the engine's
message), `zuslothdb_type_error`, `zuslothdb_unsupported`,
`zuslothdb_closed_error`.

### 5.1 The API against its two references

Read on 2026-11-17: upstream's `python/slothdb/__init__.py` at
`8180f21` (package version 0.2.6) and the installed `duckdb` 2.0.0
namespace. The rule: **the shape is `duckdb`'s, the scope is the
Python binding's**. A `duckdb` user changes `dbConnect()`'s first
argument; nothing is exported that a Python `slothdb` user would not
recognise, except what DBI requires.

<!-- markdownlint-disable MD013 -->
| Python `slothdb` | `duckdb` (R) | `zuslothdb` (R) |
| --- | --- | --- |
| `connect(path = "")` | `dbConnect(duckdb(), dbdir = ":memory:")` | `dbConnect(zuslothdb(), dbdir = ":memory:")`; the glue passes `""` to the engine |
| `Connection.sql(q)` | `dbSendQuery()`, `dbGetQuery()` | the same |
| `Connection.execute(q)` | `dbExecute()` | the same |
| `QueryResult.fetchdf()`, `.fetchnumpy()`, `.rows`, `.fetchall()`, `.fetchone()` | `dbFetch(res, n)` | `dbFetch(res, n)`: one materialisation, a data frame |
| `.columns`, `.row_count`, `len()` | `dbColumnInfo()`, `dbGetRowCount()` | the same |
| `.show()`, `repr()` | `show()` | `show()` methods for driver, connection and result |
| `Connection.close()`, `with` block | `dbDisconnect(con, shutdown = )` | the same; `withr::local_db_connection()` is the `with` block |
| `slothdb.sql(q)`, a fresh in-memory connection per call | `sql_query(sql, conn = default_conn())`, `sql_exec()` | Q10 |
| `slothdb.demo()` | none | none |
| none | `duckdb(dbdir, read_only, bigint, config)` holds the database | Q9 |
| none | `duckdb_register()`, `duckdb_unregister()` | none in v0.1.0: SlothDB has no scan over host memory, so `dbWriteTable()` copies |
| none | `duckdb_read_csv(conn, name, files)`, `read_csv_duckdb()` | `zuslothdb_read()` (§7, Q12) |
| none | `dbFetchArrow()`, `dbSendQueryArrow()`, `duckdb_register_arrow()`, `to_arrow_stream()`, `duckdb_adbc()` | later (§2) |
| none | `tbl_file()`, `tbl_query()`, `tbl_function()`, `simulate_duckdb()` | later (Q5) |
| none | `dbConnect(config = list())` | `threads` only; SlothDB has no other setting that is read (§4.2) |
| none | `dbConnect(debug, tz_out_convert, array, geometry, map)` | none |
<!-- markdownlint-enable MD013 -->

What the Python binding does that this package must not copy, all of
it inherited from the C API (§4.1) and avoided by D2: dates and
timestamps arrive as strings; an integer column with a NULL becomes
float with `NaN`; a column of any type outside the six is fetched cell
by cell as text; invalid UTF-8 is replaced, not reported; every
failure is a bare `RuntimeError("Query failed: ...")`; and
`execute()` is an alias of `sql()`, so there is no rows-affected
count. §6 and the `zuslothdb_error` classes are the answer to each;
whether the C++ API reports rows affected for `dbExecute()` is read in
Stage 2.

The Python README queries Avro, Excel and SQLite files as well as §2's
four formats (`sqlite_scan('app.db', 'users')`); Q11 asks what the R
package does with those readers.

## 6. Types

| SlothDB | R |
| --- | --- |
| `BOOLEAN` | logical |
| `TINYINT`, `SMALLINT`, `INTEGER`, `UTINYINT`, `USMALLINT` | integer |
| `BIGINT`, `UINTEGER`, `UBIGINT` | double, or `integer64` (below) |
| `HUGEINT` | error unless `bigint = "character"` (Q1) |
| `FLOAT`, `DOUBLE` | double |
| `DECIMAL` | double, or character with `decimal = "character"` |
| `VARCHAR` | character, UTF-8 |
| `BLOB` | `blob`-classed list of raw vectors |
| `DATE` | `Date` |
| `TIME` | `difftime` in seconds |
| `TIMESTAMP`, `TIMESTAMP_TZ` | `POSIXct`, UTC |
| `LIST` | list column |
| `STRUCT` | data frame column |
| `MAP`, `UNION` | error in v0.1.0 |

64-bit integers are doubles by default and `bit64::integer64` with
`bigint = "integer64"`; a value beyond 2^53 converted to double is a
`zuslothdb_type_error`, not a silent rounding.

Writing goes the other way through `Append` with `DataChunk`s built in
the glue; factors are written as `VARCHAR`. The `duckdb` choices
(`bigint`, `timezone_out`) are the defaults to copy, so that code moves
between the two drivers by changing `dbConnect()`'s first argument.

## 7. Files

SQL table functions read files, so the package adds no reader of its
own. `zuslothdb_read()` only builds the `SELECT * FROM read_<format>(...)`
call. Paths are passed as SQL string literals, escaped by
`DBI::dbQuoteString()`. Parquet decompression covers gzip, zstd and
snappy; a Parquet file with another codec is a `zuslothdb_sql_error`
from the engine, tested.

## 8. Persistence

A file database uses SlothDB's own single-file format, guarded by a
magic number in `block_manager.cpp`; the README promises "single
writer, crash-safe checkpoint" and no MVCC. The format has no
compatibility promise, so `zuslothdb_info()` reports the engine commit,
and the README tells users to treat a database file as tied to the
package version that wrote it (Q7). Opening one file from two
connections in two R processes is documented as unsupported.

## 9. Relationship to the C API and Python binding

The C API stays compiled but unexported; the glue uses it only for
`slothdb_version()`. Upstream's `python/slothdb` and npm
`@slothdb/wasm` are API references, not code to vendor. Prebuilt
binaries in the upstream tree (`packaging/npm/slothdb.wasm`,
`docs/playground/slothdb.wasm`) are never copied.

## 10. Testing and validation

- **Oracle: DuckDB.** A development-only harness (`duckdb` in
  `Suggests` of a separate test project, not the package) runs a query
  corpus through both drivers and compares results as data frames:
  types per §6, row order where `ORDER BY` fixes it, numeric tolerance
  for aggregates. Disagreements are triaged as SlothDB bugs, patched or
  listed in the README.
  The package's tests never load `duckdb`: `tools/oracle.R` runs the
  corpus (`tests/testthat/oracle/*.sql`) through `duckdb` and commits
  each answer beside its query with the `duckdb` version that gave it,
  and the tests compare `zuslothdb`'s result with the recorded one. So
  the oracle is checked on every CI leg without a source build of
  `duckdb` on any of them. The same script writes the file fixtures
  (§7) with `duckdb` as the reference writer.
- **Fuzzing.** One harness per file reader kept in the build (Q11),
  linked against the vendored engine without R. Its oracle is
  agreement, not survival: the rows a `SELECT *` returns equal
  `count(*)` over the same bytes, and a second read is identical.
- **sqllogictest.** `ZUSLOTHDB_SLOW_TESTS` runs upstream's corpus fetch
  at a pinned revision and records the pass rate per release; the
  README states it.
- **DBItest.** The `DBItest` spec for driver, connection, result and
  SQL, with skips for parameter binding listed by name.
- **Upstream unit tests.** The 408 doctest cases are rebuilt by a
  development script against the patched tree, so a patch that breaks
  one is caught before vendoring.
- **Hostile input.** Truncated and bit-flipped Parquet, CSV with
  invalid UTF-8, a corrupted database file, 10^5-deep nested
  expressions, interrupts mid-query. Each is an error, none a crash,
  under ASan, UBSan and valgrind; `ZUSLOTHDB_SKIP_HEAVY` skips the large
  cases.
- **Hygiene.** Only `R_init_zuslothdb` exported; no file written outside
  `tempdir()` and the user's `dbdir`; no thread outlives
  `dbDisconnect(shutdown = TRUE)`; `duckdb` loaded in the same session
  links and runs.

## 11. Roadmap

The stages, their order, exit criteria and CI tiers are in
[roadmap.md](roadmap.md). Stage numbers in this document are that
file's; Stage 1 is the go/no-go spike that D1 turns on.

## 12. Decisions

All are **(C)**: this RFC decides them and the maintainer may overturn
any of them.

- **D1 (C).** Build `zuslothdb` only if Stage 1 shows: it builds on
  every r-actions leg with patches of at most a few thousand lines;
  upstream's own unit tests pass on the patched tree; and a source
  install is at least several times faster than `duckdb`'s. Otherwise
  stop, and record why here. The package then exists as a test bed and
  a second engine, never as a recommended `duckdb` alternative.
- **D2 (C).** Bind SlothDB's C++ API, as `duckdb` binds DuckDB's; the
  C API's six types cannot carry §6.
- **D3 (C).** Vendor at a pinned commit, not a release tag: v0.2.7
  predates the last two weeks of fixes, and `main` has been frozen
  since 2026-06-03. Treat the vendored tree as a fork, and offer
  upstream every portability patch as a pull request.
- **D4 (C).** Offline only: no HTTP, S3, extensions or `ask` (§4.2).
- **D5 (C).** Two threads by default.
- **D6 (C).** `Imports: DBI, methods`, not `Depends: DBI` as `duckdb`
  has; users call `DBI::` or attach DBI themselves.
- **D7 (C).** No hand-written C API for other packages in v0.1.0.
- **D8 (C).** Results are materialised R vectors; ALTREP and Arrow
  export are later work.

## 13. Acceptance criteria for v0.1.0

- Every §6 type round-trips or errors as the table says; the DuckDB
  oracle agrees on the corpus except for listed differences.
- `DBItest` passes with named skips only; §10's hostile inputs error
  under the sanitizers without crashing.
- `R CMD check --as-cran` is clean on all r-actions legs, Windows and
  the r-devel containers included, offline, with no compiled-code NOTE
  and any size NOTE explained in `cran-comments.md`.
- The README states the engine commit, the patch list, the
  sqllogictest pass rate and that SlothDB is experimental.

## 14. Open questions

- **Q1.** Does SlothDB implement `HUGEINT` arithmetic, or only the type
  id? Read in Stage 4.
- **Q2.** Runtime dispatch for the AVX2 kernels, or scalar only? Depends
  on how much speed they carry, measured in Stage 1.
- **Q3.** Add prepared statements upstream-style in the fork, or wait
  for upstream? `dbplyr` and `DBI::dbAppendTable` work without them;
  `params =` does not.
- **Q4.** Implement `memory_limit` in the fork, since the field exists?
- **Q5.** Is SlothDB's SQL close enough to DuckDB's that `dbplyr`'s
  DuckDB translations can be reused through `sql_translation()`?
- **Q6.** Can a query be cancelled? The executor's thread pool must
  poll a flag; if it cannot, interrupts wait for the query to finish,
  and the README says so.
- **Q7.** Should `zuslothdb` refuse to open a database file written by a
  different engine commit, rather than trust the magic number?
- **Q8.** Name: `slothdb` matches upstream and the `duckdb` precedent,
  but suggests an official binding. Ask the upstream author, or choose
  another name. **Decided 2026-11-17: `zuslothdb`**, package, repository
  and exported names alike (`zuslothdb()`, `zuslothdb_read()`,
  `zuslothdb_error`), by the maintainer: the `zu` prefix marks it as
  this portfolio's package vendoring SlothDB, as zuyaml and zuregex
  vendor theirs, so no question to upstream is needed. The engine keeps
  its name in prose; upstream's C API (`slothdb_open()`) and Python
  module (`slothdb.sql()`) are named as upstream names them.

- **Q9.** Where does the database live? In `duckdb`, `duckdb(dbdir)`
  creates it and the driver holds it, so two `dbConnect(drv)` calls
  share one database and `dbDisconnect(shutdown = TRUE)` or
  `duckdb_shutdown(drv)` closes it. §5 writes `zuslothdb()` without
  arguments and still uses `shutdown = TRUE`, which only means
  something in that model. Proposal: copy `duckdb`,
  `zuslothdb(dbdir = ":memory:", read_only = FALSE)`, with
  `dbConnect(zuslothdb(), dbdir = )` as the short form, and no
  `zuslothdb_shutdown()` until someone needs it. Decide in Stage 2.
- **Q10.** A one-shot query function, the counterpart of Python's
  `slothdb.sql()`? Proposal: `zuslothdb_sql(sql)`, which opens an
  in-memory database, runs the query, returns the data frame and
  closes; no default connection kept in the package, unlike `duckdb`'s
  `default_conn()`. It serves the headline use (SQL over a local file)
  in one call. Not in §5 until decided.
- **Q11.** Avro, Excel and SQLite readers: compiled in and untested,
  compiled in and tested, or compiled out in `tools/vendor.R`? §2
  names only CSV, Parquet, JSON and Arrow IPC. Proposal: compiled out
  in v0.1.0, since untested parsers of hostile input are what §10
  exists to prevent. Decide in Stage 1, where the source list is made.
- **Q12.** What does `zuslothdb_read()` return, and does it take reader
  options through `...`? §5 and §7 say only that it builds the `SELECT`.
  Proposal: a data frame, as `dbGetQuery()` of that `SELECT`, with
  `format` guessed from the extension when `NULL` and no `...` in
  v0.1.0. Decide in Stage 6.

- **Q13.** What does a connection do in a forked child
  (`parallel::mclapply()`)? The engine owns threads and a file handle
  that do not survive `fork()`. Proposal: the driver records its
  process id, and any use from another process is a
  `zuslothdb_closed_error`. Decide in Stage 2.

## 15. Sources and verification

Checked on 17 November 2026:

- [SlothDB](https://github.com/SouravRoy-ETL/slothdb) at
  [`8180f21`](https://github.com/SouravRoy-ETL/slothdb/tree/8180f215847015944c935f56f008d194e2c1494c):
  `README.md`, `LICENSE`, `CMakeLists.txt`, `src/CMakeLists.txt` (the
  `-mavx2` block), `include/slothdb/api/slothdb.h`,
  `src/api/slothdb_api.cpp`, `include/slothdb/main/connection.hpp`,
  `include/slothdb/common/types/logical_type.hpp`,
  `src/common/thread_pool.cpp`, the `_MSC_VER` guards in the files of
  §4.2, `third_party/`, `bench/correctness/sqllogictest/`; Actions
  runs (every non-CodeQL run since 2026-05-22 failed); repository
  metadata (412 stars, pushed 2026-06-03).
- [duckdb-r](https://github.com/duckdb/duckdb-r) `src/` and
  `src/duckdb/` listings; crandb's `duckdb` 2.0.0 record; the CRAN
  tarball's `Content-Length`.
- `tools::CRAN_package_db()` and crandb for every package and name in
  §1.2 and the front matter.

- For §5.1: upstream `python/slothdb/__init__.py` and
  `python/README.md` at `8180f21`; `getNamespaceExports("duckdb")` and
  the formals of `duckdb()`, `dbConnect()` and `dbDisconnect()` in the
  installed `duckdb` 2.0.0.

Not verified for this draft: that the engine accepts `""` for an
in-memory database through the C++ API as it does through
`slothdb_open()`; that the tree builds anywhere, with any
compiler (Stage 1); that SlothDB's C++ API exposes a cancellation hook
(Q6); the licence headers of `unordered_dense.h` and the bitpacking
files; install time and installed size; whether arm64 builds; the
sqllogictest figure, taken from commit messages; and every engine
behaviour beyond the files named.

[admission]: https://github.com/pedrobtz/packages/blob/main/rfcs/base-layer-candidates.md#2-the-admission-test
