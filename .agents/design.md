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
  testthat (>= 3.2.0), withr, DBItest, bit64, nanoarrow, yaml` (`yaml`
  added 2026-11-17: `test-info.R` reads the installed `vendor.yaml`).
  Vendors SlothDB
  v0.2.7+ at commit `8180f21` (MIT, "SlothDB Contributors"), which
  itself carries zstd's decompressor (BSD-3-Clause, of the BSD/GPL-2.0
  dual licence), snappy (BSD-3-Clause), fastpforlib (Apache-2.0, copied
  from DuckDB), miniz 3.0.0 (MIT in `miniz.c`'s header, read
  2026-11-17; `miniz.h`'s banner also cites the Unlicense) and
  `unordered_dense.h` (MIT, read 2026-11-17). `inst/COPYRIGHTS` has each
  holder.
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

Other packages of the portfolio are adopted only where the package needs
an import (maintainer, 2026-11-17). Its imports are `DBI` and `methods`,
which none replaces, so none is adopted for v0.1.0: `qio`, `zucsv` and
`zujson` as second readers, `decimal` as a `decimal =` output and
`zuxlsx` or `zuarrow` as fixture writers were considered and set aside.

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
`LIST`, `MAP`, `STRUCT` and `UNION`; most of those are type ids the
engine never produces (§6, *as found in Stage 4*). `duckdb` binds DuckDB's C++ API for
the same reasons. D2 takes the C++ API; the price is that every
upstream refactor is a glue change, which a pinned, patched fork makes
a choice rather than a surprise.

### 4.2 Patches

Each is recorded in `tools/vendor.yaml` with its reason. If they
together pass a few thousand lines, or Stage 1 finds the engine wrong
on its own tests, D1's stop condition applies.

**As built in Stage 1 (2026-11-17):** six patches in `tools/patches/`,
153 changed lines (71 added, 82 removed), plus one patch of 20 lines to
upstream's tests. `tools/vendor.R` applies them to upstream at the
pinned commit and copies the result, minus `src/ask/`, `src/extension/`
the HTTP client and the C API, into `src/vendor/slothdb/`; `tools/vendor/verify`
rebuilds that tree and compares it file by file. Against the plan
below: the MinGW patch needed only the time functions in
`expression_executor.cpp` and `mkdir` in `hive_partition.cpp`, since
MinGW has `access`, `fseeko`, `dirent.h` and `stat`, and the files with
`dlfcn.h`, sockets and `#pragma comment` are compiled out; a MinGW-w64
syntax pass over the whole tree is clean. The baseline-ISA change needs
no patch (Q2). Two findings the plan did not have: `rand()` in
`RANDOM()` and `UUID()` (patch 0002), and `STRFTIME` with one argument
reading past its argument list (patch 0005); and two compiler warnings
R CMD check counts as significant (patch 0006), the third such warning
being in the C API, which is left out instead. Symbols: `-fvisibility=hidden`
through `$(CXX_VISIBILITY)` and `$(C_VISIBILITY)`, zstd's own
`visibility("default")` emptied with `-DZSTDLIB_VISIBLE=`, and
`src/zuslothdb-win.def` on Windows. The C++ standard library's template
typeinfo stays exported on macOS and Linux, as it does from every C++
library; it is identical wherever it is instantiated and cannot clash.

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
  As built (patch 0004): one process-wide cap, 2 by default, read by
  every pool and split. The engine's thread pool is a process global
  sized on first use, so `threads` is per process, not per database
  as in `duckdb`. **Decided 2026-11-17** (maintainer): it stays
  `dbConnect(threads = 2L)`, documented as the process-wide cap that
  the last `dbConnect()` sets; `zuslothdb_info()` reports it. The pool
  is sized to the smaller of the cap and the hardware's threads.
- **Read-only (Stage 2).** The engine has no read-only mode and saves
  a file database whenever it closes. Patch 0007 adds
  `DatabaseConfig::read_only`: `Connection::Query` refuses a batch
  holding any statement that changes the database, before any of it
  runs, and the destructor does not save.
- **Lost wakeup (found 2026-11-17, #32).** `PhysicalParquetScan` and
  `ThreadPool` set their stop flag outside the mutex their workers wait
  with. A worker between its predicate and its wait missed the notify,
  and `join()` never returned: a Parquet query that stopped early hung
  the session, and so, 1 run in about 14, did upstream's doctests.
  Patch 0008 sets each flag under that mutex.
- **`thread_local` destructors on Windows (found 2026-11-18, #34).**
  The Parquet decoder keeps scratch buffers and a zstd context in
  `thread_local` objects with destructors. Under MinGW those
  destructors run as a thread exits (winpthreads' key destructors,
  through libstdc++'s `__cxa_thread_atexit`), after emulated TLS has
  freed the objects, and the heap is corrupted: every Parquet scan with
  decode workers, so on two threads or more, killed R with
  `0xC0000409`. Only decode workers start and end within a query; the
  main thread and the pool never exit mid-session. Patch 0009 makes
  the seven objects local to their call. A `thread_local` with a
  destructor is not to be added in a patch.
- **Validity past one vector (found 2026-11-18, #45).** `ValidityMask`
  held 2048 bits, one vector's, and a row group's column (122880 rows)
  kept its `NULL`s in one: a `NULL` from row 2049 on was written past a
  heap buffer, and others read back as `NULL` (5 `NULL`s in 5000 rows
  read as 817). `PhysicalInsert` also put a whole `VALUES` list into one
  2048-row chunk. Patch 0010 sizes the mask by its holder's capacity and
  the chunk by the list. 31 other chunks keep the default capacity and
  are not yet audited; one filled past 2048 rows would be the same bug.
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
                 timezone_out = "UTC")
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
and `show`. `bigint` and `timezone_out` are §6's options; `bigint`
also takes `"integer64"` and `"character"`. `decimal` is not added in
v0.1.0, since the engine produces no `DECIMAL` (§6, 2026-11-18).

`dbGetQuery(params = )` and `dbBind()` raise `zuslothdb_unsupported` in
v0.1.0, since SlothDB has no prepared statements (Q3). Conditions
inherit from `zuslothdb_error`: `zuslothdb_sql_error` (the engine's
message), `zuslothdb_type_error`, `zuslothdb_unsupported`,
`zuslothdb_closed_error`, and `zuslothdb_argument_error` for a bad
argument (added 2026-11-17, Stage 2, so a caller can tell an argument
from an engine error).

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
Stage 2. *Read 2026-11-17:*
it does: `INSERT`, `UPDATE` and `DELETE` return one `BIGINT` column
named `Count`, which `dbExecute()` returns; any other statement
affected 0 rows.

The Python README queries Avro, Excel and SQLite files as well as §2's
four formats (`sqlite_scan('app.db', 'users')`); Q11 asks what the R
package does with those readers.

## 6. Types

| SlothDB | R |
| --- | --- |
| `BOOLEAN` | logical |
| `TINYINT`, `SMALLINT`, `INTEGER`, `UTINYINT`, `USMALLINT` | integer |
| `BIGINT`, `UINTEGER`, `UBIGINT` | double, or `integer64` (below) |
| `HUGEINT` | error (Q1) |
| `FLOAT`, `DOUBLE` | double |
| `DECIMAL` | error in v0.1.0: the engine produces none |
| `VARCHAR` | character, UTF-8 |
| `BLOB` | `blob`-classed list of raw vectors |
| `DATE` | `Date` |
| `TIME` | error in v0.1.0: every value the engine makes is midnight (#43) |
| `TIMESTAMP` | `POSIXct`, its `tzone` set by `timezone_out` |
| `TIMESTAMP_TZ`, `LIST`, `STRUCT`, `MAP`, `UNION` | error in v0.1.0: the engine produces none |
| `INTERVAL`, `UUID` | error in v0.1.0 (Q14) |

64-bit integers are doubles by default and `bit64::integer64` with
`bigint = "integer64"`; a value beyond 2^53 converted to double is a
`zuslothdb_type_error`, not a silent rounding.

*As built in Stage 3 (2026-11-17)*, the same rule for the values R holds
only by losing them: an `INTEGER` of -2^31 (R's `NA_integer_`), a
`BIGINT` of -2^63 under `bigint = "integer64"` (bit64's `NA`), a
`UBIGINT` past 2^63 - 1 under `"integer64"`, and a string that is not
valid UTF-8 or holds a NUL are each a `zuslothdb_type_error` naming the
column and row, and nothing of the slice is fetched. `bigint` also takes
`"character"`, the decimal digits. A bare `NULL` arrives as an integer
`NA`, as in `duckdb`. Results are computed whole and copied into plain
buffers before any R vector is made (`src/sdb_result.cpp`), and a
connection has one open result: sending another clears it with a
warning, as RSQLite does. The unsigned types cannot be named in
SlothDB's SQL, so they reach R only from files (Stage 6).

*As found in Stage 4 (2026-11-18)*, the engine produces far fewer types
than its `LogicalTypeId` names. Its SQL casts a `DECIMAL` to `DOUBLE`
and a column declared `TIMESTAMPTZ` to `TIMESTAMP`; it has no list,
struct or map syntax, and `LIST_VALUE`, `STRUCT_PACK` and `ROW` are
"not implemented". Its readers do no better: Parquet reads a `DECIMAL`
as its unscaled integer (#36) and sees no nested column, JSON drops
nested keys (#39), and Arrow IPC with a nested column fails (#40). So
the rows for `DECIMAL`, `TIMESTAMP_TZ`, `LIST`, `STRUCT`, `MAP` and
`UNION` above, which this design first promised as conversions, are a
`zuslothdb_type_error` naming the column, for a value no route yet
delivers, and `dbConnect(decimal = )` is not added (maintainer,
2026-11-18). They become conversions when the engine produces the type,
by upstream or by a patch. `HUGEINT` values reach R only through
`CAST(column AS HUGEINT)`, and they are wrong (Q1), so it is an error
whatever `bigint` says. `TIME` is the same case: every `TIME` the
engine makes, by a cast or from a `TIMESTAMP`, is `00:00:00`, its
literal is unimplemented and Parquet reads one as a `BIGINT` count
(#43, #37), so a `TIME` column is an error until the engine computes
one; it is then a `difftime` in seconds. `DATE` is a `Date` and
`TIMESTAMP` a `POSIXct`, the engine's microseconds as
seconds since 1970-01-01 UTC, its `tzone` attribute `timezone_out`
(`"UTC"` by default; `""` is the session's time zone): the same instant
whatever the zone, as `duckdb`'s default `tz_out_convert = "with"`.
`timezone_out` is a time zone from `OlsonNames()` or `""`; another is a
`zuslothdb_argument_error`, where `duckdb` warns and falls back to UTC.
A `BLOB` is a list of raw vectors, `NULL` for a missing value, with the
classes of `blob::blob()`, set without importing `blob`.

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

*As read in Stage 2 (2026-11-17):* there is no write-ahead log. A file
database is read whole into memory when it opens (`Checkpoint::Load`)
and written whole when the `Database` is destroyed, through a `.tmp`
file and a rename beside it. So a crash loses every change since the
database opened, and two drivers on one file, in one process or two,
each write their own copy at close: the last to close wins. Stage 6
documents both and tests the round trip; v0.1.0 does not lock the
file.

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
  links and runs. That last check runs in `tools/ci/platform.sh` on
  macOS, Windows and Ubuntu, with a `duckdb` binary installed on the
  runner, so `duckdb` stays out of `Suggests` (confirmed 2026-11-17: the
  recorded answers and the CI step are the only uses, and neither needs
  it at check time).

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
  **Recorded 2026-11-17 (Stage 1): go.** Each condition, as measured:
  - *Patches:* six, 153 changed lines (71 added, 82 removed), and one
    of 20 lines to upstream's tests (§4.2).
  - *Upstream's tests:* unpatched, the doctest suite crashed at its 95th
    case (`STRFTIME` with one argument) and, with that case excluded,
    failed 7 of 407 (stale DATE encodings). On the patched tree, all
    378 cases built (the 30 of the two compiled-out modules excluded)
    pass, 131,473 assertions, on macOS with R's compiler, and on every
    `platforms.yaml` leg (`tools/upstream-tests.sh`).
  - *Install time:* a cold, serial source install from the tarball on
    the same 6-core x86-64 Mac took 187 s for `zuslothdb` and 2,711 s
    for `duckdb` 2.0.0, 14.5 times as long. The tarball is 0.82 MB
    against 8.3 MB, the installed package 3.9 MB against 72 MB.
  - *Every leg:* the full r-actions profile on the Stage 1 pull request
    (#28), Rtools and the r-devel containers included.
  Stage 3's oracle found more (`tests/testthat/oracle/known-differences.md`):
  `substr()` cuts bytes, not characters; TINYINT arithmetic does not
  check overflow; a subnormal double literal is refused; infinity
  inserted into a FLOAT column is refused.
  Also found: a table function inside a subquery is not bound
  (`SELECT count(*) FROM (SELECT ... FROM read_parquet(...))` is
  "Table 'READ_PARQUET' not found"), the kind of gap the sqllogictest
  figure of about 18% predicts; Stages 3 and 6 list such cases in
  `known-differences.md`.
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
  id? Read in Stage 4. **Answered 2026-11-18: only the type id.** A
  128-bit literal is refused and a constant cast to `HUGEINT` stays an
  `INTEGER`; `CAST(column AS HUGEINT)` gives the type, with wrong
  values: 42 reads as 16, and (2^63 - 1) + (2^63 - 1) as 0. So a
  `HUGEINT` column is a `zuslothdb_type_error`, with no
  `bigint = "character"` exception (§6).
- **Q2.** Runtime dispatch for the AVX2 kernels, or scalar only? Depends
  on how much speed they carry, measured in Stage 1. **Decided
  2026-11-17: scalar only for v0.1.0**, by the maintainer. Every
  intrinsic path is guarded by `__AVX2__` or `__SSE2__`, so building
  without `-mavx2` selects the scalar code with no patch; SSE2 is the
  x86-64 baseline and stays. Measured on a 6-core x86-64 Mac (two
  engine threads, 10^7 rows written by `duckdb`, median of 5): the AVX2
  build (`-mavx2 -mbmi2`) was 0 to 6% faster across seven queries, about
  5% on a filtered Parquet sum, 3% on a CSV `GROUP BY` and nothing on
  the rest, inside run-to-run noise. Runtime dispatch is not worth its
  patch for v0.1.0.
- **Q3.** Add prepared statements upstream-style in the fork, or wait
  for upstream? `dbplyr` and `DBI::dbAppendTable` work without them;
  `params =` does not.
- **Q4.** Implement `memory_limit` in the fork, since the field exists?
- **Q5.** Is SlothDB's SQL close enough to DuckDB's that `dbplyr`'s
  DuckDB translations can be reused through `sql_translation()`?
- **Q6.** Can a query be cancelled? The executor's thread pool must
  poll a flag; if it cannot, interrupts wait for the query to finish,
  and the README says so. **Answered 2026-11-17: no.** Nothing in the
  engine polls an interrupt or cancel flag (no such identifier in
  `src/` or `include/`), and `Connection::Query()` runs to completion
  on the calling thread. Adding one means a patch in every operator
  loop of a 14,573-line planner, outside D1's budget for v0.1.0. So an
  interrupt waits for the query to finish; Stage 3 has nothing to wire
  and Stage 8's README says so.
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
  **Decided 2026-11-17: copy `duckdb`**, by the maintainer. As built:
  the driver holds the database and opens it at its first connection;
  connections from one driver share it; `dbConnect(drv, dbdir = )` or
  `read_only = ` that differ from the driver's give a new driver.
  `dbDisconnect(con)`, with `shutdown = TRUE` as in `duckdb` 2.0, closes
  the database with its *last* open connection (writing a file), and
  never closes other open connections; `shutdown = FALSE` keeps it open
  for the driver's next connection, and the driver's next
  `dbConnect()` reopens a closed one. A connection or a kept-open
  database collected by the garbage collector is closed with a warning.
  This is deterministic where `duckdb` 2.0 closes on its last
  reference, because a file is written only when its database closes.
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
  **Decided 2026-11-17: compiled in and tested**, by the maintainer.
  The three readers are in the source list; Stage 6 gives them what it
  gives the other formats (fixtures with recorded answers, hostile
  inputs, a fuzz harness each), and §2 and §7 gain them there.
- **Q12.** What does `zuslothdb_read()` return, and does it take reader
  options through `...`? §5 and §7 say only that it builds the `SELECT`.
  Proposal: a data frame, as `dbGetQuery()` of that `SELECT`, with
  `format` guessed from the extension when `NULL` and no `...` in
  v0.1.0. Decide in Stage 6.

- **Q13.** What does a connection do in a forked child
  (`parallel::mclapply()`)? The engine owns threads and a file handle
  that do not survive `fork()`. Proposal: the driver records its
  process id, and any use from another process is a
  `zuslothdb_closed_error`. Decide in Stage 2. **Decided 2026-11-17: as
  proposed**, by the maintainer. Each database and connection records
  the process that made it; from any other, `dbIsValid()` is `FALSE`
  and every call a `zuslothdb_closed_error`, and its finalizers do
  nothing, so a child's exit never writes the parent's file.

- **Q14.** `INTERVAL` and `UUID` columns can be declared, and the
  engine also names `ENUM` and `ARRAY`; §6 has no row for them. Stage 4
  (2026-11-18) leaves them a `zuslothdb_type_error`. What should each
  arrive as? `duckdb`'s answer is the reference to copy, as for §6's
  other rows. Parquet reads `UUID` and `INTERVAL` as `NULL` (#38).

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
