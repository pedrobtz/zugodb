# zuslothdb: roadmap

Status: draft 2026-11-17, restaged 2026-11-18 from Stage 6 on (design
D9). Implements [design.md](design.md).
Target: v0.1.0, the first CRAN release.
Tracking: [v0.1.0 (#1)](https://github.com/pedrobtz/zuslothdb/issues/1),
with one sub-issue per stage;
[CRAN-submission (#15)](https://github.com/pedrobtz/zuslothdb/issues/15).

Stage 1 is a go/no-go spike. If its numbers meet the stop condition of
[D1](design.md#12-decisions), the package stops there: the reason is
recorded in the design and the remaining stage issues are closed as not
planned.

**Restaged 2026-11-18.** D1 was looked at again on the evidence in
[slothdb-assessment.md](slothdb-assessment.md): the package goes on, by
another route. The engine's file readers are refused and the zu family's
readers take their place ([design §7](design.md#7-files), D9), and the
engine's core is measured and fixed on the fork. Stage 6 was rewritten,
Stages 7, 8 and 10 are new, and the old Stages 7 to 12 are now 9 and 11
to 15. v0.1.0 has no CRAN date
([Q18](design.md#14-open-questions), decided 2026-11-18): it is submitted
when Stage 8's bar holds, and until then the package is installed from
GitHub or the maintainer's r-universe.

## Stages

<!-- markdownlint-disable MD013 -->
| Stage | Title | CI | Issue | Pull request | Done |
| --- | --- | --- | --- | --- | --- |
| 0 | [Package identity](#stage-0--package-identity) | quick | [#2](https://github.com/pedrobtz/zuslothdb/issues/2) | [#27](https://github.com/pedrobtz/zuslothdb/pull/27) | 2026-11-17 |
| 1 | [Engine spike](#stage-1--engine-spike) | full | [#3](https://github.com/pedrobtz/zuslothdb/issues/3) | [#28](https://github.com/pedrobtz/zuslothdb/pull/28) | 2026-11-17 |
| 2 | [Driver and connection](#stage-2--driver-and-connection) | full | [#4](https://github.com/pedrobtz/zuslothdb/issues/4) | [#30](https://github.com/pedrobtz/zuslothdb/pull/30) | 2026-11-17 |
| 3 | [Results and scalar types](#stage-3--results-and-scalar-types) | full | [#5](https://github.com/pedrobtz/zuslothdb/issues/5) | [#31](https://github.com/pedrobtz/zuslothdb/pull/31) | 2026-11-17 |
| 4 | [Temporal, decimal and nested types](#stage-4--temporal-decimal-and-nested-types) | quick | [#6](https://github.com/pedrobtz/zuslothdb/issues/6) | [#44](https://github.com/pedrobtz/zuslothdb/pull/44) | 2026-11-18 |
| 5 | [Writing and transactions](#stage-5--writing-and-transactions) | quick | [#7](https://github.com/pedrobtz/zuslothdb/issues/7) | [#50](https://github.com/pedrobtz/zuslothdb/pull/50) | 2026-11-18 |
| 6 | [Persistence and the engine's own readers](#stage-6--persistence-and-the-engines-own-readers) | full | [#8](https://github.com/pedrobtz/zuslothdb/issues/8) | | |
| 7 | [Arrow streams in](#stage-7--arrow-streams-in) | full | [#58](https://github.com/pedrobtz/zuslothdb/issues/58) | | |
| 8 | [Core conformance](#stage-8--core-conformance) | full | [#59](https://github.com/pedrobtz/zuslothdb/issues/59) | | |
| 9 | [DBItest conformance](#stage-9--dbitest-conformance) | quick | [#9](https://github.com/pedrobtz/zuslothdb/issues/9) | | |
| 10 | [Readers from the zu family](#stage-10--readers-from-the-zu-family) | full | [#60](https://github.com/pedrobtz/zuslothdb/issues/60) | | |
| 11 | [Documentation and measured claims](#stage-11--documentation-and-measured-claims) | quick | [#10](https://github.com/pedrobtz/zuslothdb/issues/10) | | |
| 12 | [Assurance](#stage-12--assurance) | quick | [#11](https://github.com/pedrobtz/zuslothdb/issues/11) | | |
| 13 | [Full CI](#stage-13--full-ci) | full | [#12](https://github.com/pedrobtz/zuslothdb/issues/12) | | |
| 14 | [v0.1.0 readiness](#stage-14--v010-readiness) | full | [#13](https://github.com/pedrobtz/zuslothdb/issues/13) | | |
| 15 | [Review](#stage-15--review) | full | [#14](https://github.com/pedrobtz/zuslothdb/issues/14) | | |
<!-- markdownlint-enable MD013 -->

## Stage 0 — Package identity

**Issue.** [#2](https://github.com/pedrobtz/zuslothdb/issues/2), sub-issue of
[v0.1.0](https://github.com/pedrobtz/zuslothdb/issues/1).

**CI.** `quick`.

**Goal.** The repository is a named, licensed, checked package with its CI
economy and its assurance file in place, before any engine code arrives.

**Work.**

- `DESCRIPTION`: real `Title`, `Description` and `Authors@R` (`cre`, `aut`,
  `cph`), `Language: en-GB`, `Config/roxygen2/version: 8.1.0`. The README's
  first paragraph says what the package is and that the engine is
  experimental.
- `R-CMD-check.yaml` gains the Windows R-devel runner row; `pkgdown.yaml`
  becomes a caller of r-actions `pkgdown.yml`; `assurance.yaml` calls
  `assurance.yml` with `ungated-jobs: fail`. A free weekly schedule slot
  is picked for each.
- Labels `stage`, `full-ci`, `ci:minimal` and `ci:docs` created.
- `.agents/assurance.md` from the template, kinds `compiled`,
  `untrusted-input`, `spec`, `lifecycle` and `emits`, every check and row
  not yet backed written as a tracked gap.
- The test runner CI uses: `tools/ci-run-tests.R`, shuffled, printing its
  seed, with coverage on.
- `tools/ci/platform.sh` as the one place a per-OS step goes, called by a
  `platforms.yaml` caller; empty of steps for now.

**Tests.**

- The package loads and its one placeholder test runs shuffled: the seed
  printed in the CI log, in `tests/testthat/test-zuslothdb.R`.
- `.agents/assurance.md` is well formed and every gap is tracked:
  `assurance.yml`, which fails on either.

**Exit criteria.**

- `R CMD check` passes on the quick r-actions profile, and the pull
  request's quick run finishes in under five minutes.
- `assurance.yaml` is green.
- `grep -c 'What the package does' DESCRIPTION` prints 0.

**Design.** [§3](design.md#3-position-in-the-portfolio) of design.md, for
the family conventions that apply.

## Stage 1 — Engine spike

**Issue.** [#3](https://github.com/pedrobtz/zuslothdb/issues/3), sub-issue of
[v0.1.0](https://github.com/pedrobtz/zuslothdb/issues/1).

**CI.** `full — a vendored C++20 library and the package's first compiled
code: every platform leg, sanitizers, valgrind and rchk must see it before
anything is built on it.`

**Goal.** The patched engine compiles inside the package on every CI leg
and answers one query, and the numbers that decide D1 are written down.

**Work.**

- `tools/vendor.R` and `tools/vendor.yaml`: copy upstream at the pinned
  commit, apply the patches, write the source list into `src/Makevars`
  and `Makevars.win`. `CXX_STD = CXX20`; the C stub goes.
- The patches of §4.2, with Q2 (AVX2 kernels) and Q11 (which readers are
  compiled) decided and recorded.
- Minimal glue: one database, one connection, `SELECT 42` returned as an
  R integer, behind the exception barrier of §4.3. `zuslothdb_info()`.
- Symbol visibility: only `R_init_zuslothdb` exported.
- `tools/upstream-tests.sh`: builds upstream's doctest binary against the
  patched tree without R, run as a step of `tools/ci/platform.sh`.
- `native-checks.yaml` (sanitizers, valgrind, rchk, analyzers) and
  `gctorture.yaml` callers.
- `inst/COPYRIGHTS` and `Authors@R` `cph` entries for SlothDB and each
  vendored third-party library, with the licence headers the design lists
  as unverified read.
- The measurements, written into the design: patch size in lines, the
  doctest result, cold source-install time against `duckdb`'s on the same
  runner, tarball and installed size, and whether a query can be
  cancelled (Q6).

**Tests.**

- `SELECT 42`: the literal, in `tests/testthat/test-spike.R`.
- `zuslothdb_info()` reports the pinned commit and patch list:
  `tools/vendor.yaml`, read by the test, in `test-info.R`.
- The patches break nothing upstream tested: upstream's own doctest
  cases, by `tools/upstream-tests.sh`.
- Only `R_init_zuslothdb` is exported: `nm` on the built library, in
  `tools/ci/platform.sh`, once per OS.
- `duckdb` loaded in the same session: both answer `SELECT 42`, in
  `test-coexist.R`, skipped without `duckdb`. *Deviation 2026-11-17:* in
  `tools/ci/coexist.R`, run by `tools/ci/platform.sh` on each OS in both
  load orders, with a Parquet round trip through DuckDB's own
  fastpforlib. A testthat file using `duckdb` must declare it in
  `Suggests`, which §10 rules out, and would be skipped on every
  `R CMD check` leg anyway.
- The MinGW branch and the baseline-ISA build: the Windows legs and the
  r-actions x86-64 legs, which run without AVX2 flags.

**Exit criteria.**

- A full-profile run is green on the pull request, Rtools and the r-devel
  containers included.
- Every `**Tests.**` line exists and was seen to fail.
- D1 is recorded in the design as go or stop, with the measurements.
- The quick profile's duration with the engine compiled is recorded here;
  if over five minutes, `quick-containers` is trimmed and the change noted.
  *Recorded 2026-11-17 (#28):* serial, the quick legs took 7 m 47 s
  (Ubuntu release) and 6 m 49 s (clang23), nearly all of it the engine's
  build. Trimming `quick-containers` shortens no check, so instead
  `MAKEFLAGS=-j4` goes to R-CMD-check, sanitizers and gctorture:
  4 m 32 s and 4 m 53 s. UBSan stays at 10 to 14 minutes, and valgrind,
  rchk, LTO and the analysers, which take no `env` input, at 4 to 10.

**Design.** [§4](design.md#4-architecture),
[§4.2](design.md#42-patches), [§1.3](design.md#13-upstream-health) and
[D1 to D5](design.md#12-decisions).

## Stage 2 — Driver and connection

**Issue.** [#4](https://github.com/pedrobtz/zuslothdb/issues/4), sub-issue of
[v0.1.0](https://github.com/pedrobtz/zuslothdb/issues/1).

**CI.** `full — external pointers with finalizers for the database and
the connection: valgrind, ASan and a dispatched step-20 gctorture run.`

**Goal.** `dbConnect(zuslothdb())` and `dbDisconnect()` work, every
lifetime is defined, and failures are classed conditions.

**Work.**

- `zuslothdb()`, `dbConnect()`, `dbDisconnect()`, `dbIsValid()`,
  `dbGetInfo()`, `show()`; the `zuslothdb_driver` and `zuslothdb_connection`
  classes; `threads`, `read_only`.
- Q9 (where the database lives) and Q13 (fork) decided and recorded.
- The `zuslothdb_error` condition classes.
- `dbExecute()` for statements without a result, so tests can set up
  state.
- The Objects rows for database and connection in `.agents/assurance.md`.

**Tests.**

- Database and connection, out-of-order sequences (disconnect twice, use
  after disconnect, use after `shutdown`, database collected while a
  connection lives, collected without disconnect): each a
  `zuslothdb_closed_error` or the documented warning, in
  `test-lifecycle.R`. *Note 2026-11-17:* R runs finalizers through
  `R_ToplevelExec`, so `expect_warning(gc())` cannot see their warning;
  collection is tested by its effect, the warning by calling the
  finalizer.
- Fork guard: a connection used in a `parallel::mcparallel()` child, in
  `test-fork.R`, skipped on Windows.
- Unwind guard: an R error raised during a call leaves the connection
  valid and no engine object leaked, checked under valgrind, in
  `test-unwind.R`.
- `threads`: 0 refused, 1 and 2 accepted and reported by
  `zuslothdb_info()`, the default 2, in `test-connect.R`.
- `read_only = TRUE`: a write is a `zuslothdb_sql_error` and the file's
  bytes are unchanged (`tools::md5sum()`), in `test-connect.R`.
- An engine error: class `zuslothdb_sql_error` carrying the engine's
  message, in `test-conditions.R`.

**Exit criteria.**

- A full-profile run and a dispatched step-20 gctorture run are green on
  the pull request.
- Every `**Tests.**` line exists and was seen to fail.

**Design.** [§4.3](design.md#43-exceptions-interrupts-and-lifetimes),
[§5](design.md#5-public-r-api-proposed) and
[§5.1](design.md#51-the-api-against-its-two-references).

## Stage 3 — Results and scalar types

**Issue.** [#5](https://github.com/pedrobtz/zuslothdb/issues/5), sub-issue of
[v0.1.0](https://github.com/pedrobtz/zuslothdb/issues/1).

**CI.** `full — the column conversion from engine chunks to R vectors is
where the glue allocates and protects: sanitizers, valgrind, rchk and
step-20 gctorture.`

**Goal.** Queries return data frames whose scalar columns are exactly the
R types the design's table names.

**Work.**

- `dbSendQuery()`, `dbFetch(n)` by chunk, `dbClearResult()`,
  `dbGetQuery()`, `dbHasCompleted()`, `dbGetRowCount()`,
  `dbGetRowsAffected()`, `dbGetStatement()`, `dbColumnInfo()`; the
  `zuslothdb_result` class.
- Boolean, integer, floating-point and `VARCHAR` columns; `bigint`.
- `params =` and `dbBind()` raise `zuslothdb_unsupported`.
- The oracle harness: `tools/oracle.R` and `tests/testthat/oracle/`, each
  query with the `duckdb` answer recorded beside it and the `duckdb`
  version that gave it.
- `alloc-failure.yaml` caller.
- Interrupts, as Stage 1 found them possible (Q6). *Stage 1 found them
  not possible (2026-11-17): nothing to wire here.*
- The Objects row for results and the Limits row for 2^53.

**Tests.**

- Each scalar type, with NULLs, empty results and results longer than one
  chunk: the recorded `duckdb` answers, in `test-oracle-scalar.R`.
- `dbFetch(n)`: chunks of `n - 1`, `n` and `n + 1` rows against a known
  row count concatenate to the `dbGetQuery()` result, in `test-fetch.R`.
- 64-bit integers at 2^53 - 1, 2^53 and 2^53 + 1 under each `bigint`
  value: exact value or `zuslothdb_type_error`, against the literal's
  decimal string, in `test-bigint.R`.
- `VARCHAR`: UTF-8 marked, embedded multi-byte text identical to the
  input literal, in `test-varchar.R`.
- Result sequences (fetch after clear, clear twice, fetch after
  disconnect, a second query while one is open): in `test-lifecycle.R`.
- `params =`: `zuslothdb_unsupported`, in `test-conditions.R`.

**Exit criteria.**

- A full-profile run and a dispatched step-20 gctorture run are green on
  the pull request.
- Every `**Tests.**` line exists and was seen to fail; every disagreement
  with a recorded answer is patched or listed in
  `tests/testthat/oracle/known-differences.md`.

**Design.** [§6](design.md#6-types),
[§10](design.md#10-testing-and-validation) and
[D8](design.md#12-decisions).

## Stage 4 — Temporal, decimal and nested types

**Issue.** [#6](https://github.com/pedrobtz/zuslothdb/issues/6), sub-issue of
[v0.1.0](https://github.com/pedrobtz/zuslothdb/issues/1).

**CI.** `quick`.

**Goal.** Every remaining row of the design's type table converts or
errors as the table says.

**Deviation (2026-11-18, approved by the maintainer).** The engine
produces no `DECIMAL`, `TIMESTAMP_TZ`, `LIST`, `STRUCT`, `MAP` or
`UNION` by any route, and its `HUGEINT` values are wrong (design §6, Q1).
So the stage converts the types the engine delivers, and the rest are
errors; `decimal =` is not added, and the nested-type tests are dropped
with it. The readers' wrong answers found here are Stage 6's (#36 to
#40).

**Work.**

- `DATE` and `TIMESTAMP`, with `dbConnect(timezone_out = )`; `BLOB`. A
  `zuslothdb_type_error` naming the column for `HUGEINT`, `TIME`,
  `INTERVAL` and `UUID`, the types SQL can reach that do not convert:
  `TIME` because every value the engine makes is midnight (#43, found
  2026-11-18 while building the stage). Q1 read and recorded.

**Tests.**

- Each type with NULLs, through a typed table and through a set
  operation's boxed rows: the recorded `duckdb` answers, in
  `test-oracle-types.R`.
- Dates and timestamps before 1970, on leap days, to the microsecond and
  at the type's extremes: the recorded answers, in `test-temporal.R`.
- `timezone_out`: the same instants under another zone and `""`, and an
  unknown zone a `zuslothdb_argument_error`, in `test-temporal.R`.
- `HUGEINT`, `TIME`, `INTERVAL`, `UUID`: `zuslothdb_type_error` naming
  the column, in `test-conditions.R`; `TIME` and the `BLOB` escape held
  as known differences (`oracle/diff-time-cast`, `diff-blob-escape`).

**Exit criteria.**

- `R CMD check` passes on the quick profile.
- Every `**Tests.**` line exists and was seen to fail.

**Design.** [§6](design.md#6-types).

## Stage 5 — Writing and transactions

**Issue.** [#7](https://github.com/pedrobtz/zuslothdb/issues/7), sub-issue of
[v0.1.0](https://github.com/pedrobtz/zuslothdb/issues/1).

**CI.** `quick`.

**Goal.** Data frames go into tables and come back identical, and
transactions commit and roll back.

**Deviation (2026-11-18, approved by the maintainer).** SlothDB has no
transactions: `ROLLBACK` undoes nothing (design §5). So `dbBegin()`,
`dbCommit()` and `dbRollback()` are `zuslothdb_unsupported`, SQL
`ROLLBACK` is an error by the fork's commit `6af32c5` (`zuslothdb-2`),
and the rollback test becomes one that the refusals hold and that a
refused write leaves no row. Building the stage also found that a table
past 2048 rows with a `NULL` corrupted memory (#45, fixed first).

**Work.**

- `dbWriteTable()`, `dbAppendTable()`, `dbDataType()`,
  `dbQuoteIdentifier()`, `dbListTables()`, `dbListFields()`,
  `dbExistsTable()`, `dbRemoveTable()`; `dbBegin()`, `dbCommit()`,
  `dbRollback()`.

**Tests.**

- Round trip of every type the table writes, with `NA`, zero rows, and
  more rows than one chunk: `identical()` to the input, in
  `test-roundtrip.R`.
- Factors arrive as `VARCHAR`; a column of a type with no mapping is a
  `zuslothdb_type_error` and the table is unchanged, in `test-write.R`.
- Identifiers that need quoting (spaces, quotes, reserved words, UTF-8):
  round trip through `dbListFields()`, in `test-identifiers.R`.
- `dbBegin()`, `dbCommit()`, `dbRollback()` and `dbWithTransaction()`
  `zuslothdb_unsupported`, and SQL `ROLLBACK` an error that keeps the
  rows, in `test-transactions.R`; a value that does not fit, at row 5000
  of an append, leaves no row, in `test-write.R`.
- Transaction sequences (commit without begin, begin twice, disconnect
  inside a transaction): in `test-lifecycle.R`.

**Exit criteria.**

- `R CMD check` passes on the quick profile.
- Every `**Tests.**` line exists and was seen to fail.

**Design.** [§5](design.md#5-public-r-api-proposed) and
[§6](design.md#6-types).

## Stage 6 — Persistence and the engine's own readers

**Issue.** [#8](https://github.com/pedrobtz/zuslothdb/issues/8), sub-issue of
[v0.1.0](https://github.com/pedrobtz/zuslothdb/issues/1). Retitled
2026-11-18 from "Files and persistence".

**CI.** `full — fork commits to the engine, and the database file is
bytes the package did not write: ASan, valgrind and its fuzz harness.`

**Goal.** A database file survives a restart and a damaged one is an
error; no query reaches one of the engine's file readers; and an error
on an engine worker thread is an error, not the end of the session.

**Restaged (2026-11-18, design D9).** The stage first covered reading
files through the engine's readers. Those are refused here, and files
come back in Stage 10 through the zu family's readers. What the stage had
already landed stays: #36 to #40 (fork changes 13 to 16) and #55 (pull
request #56).

**Work.**

- The database file's format version, as Q7 decided: a classed error for
  another version, zuslothdb's range from 1001 by a fork commit, and a
  golden database file read back in CI.
- As Q17 decided: a fork commit refuses the engine's CSV, Parquet and
  JSON readers, as #56 does SQLite, Excel and Avro, with an error that
  names `zuslothdb_read()`. Cutting them is Stage 10's.
- Upstream's doctest cases that read files become checks of the
  refusals, or go to `test_exclude` with the reason.
- The package's tests that read files through SQL
  (`test-parquet-types.R`, `test-parquet-stop.R`, `test-json.R`,
  `test-arrow.R`) become refusal tests. Their fixtures and recorded
  answers are kept for Stage 10.
- A fork commit: an exception on an engine worker thread is caught there
  and re-thrown on the query's thread after the join (the assessment's
  A1).
- A checksum over the database file, written and checked by the fork
  commit that takes the format version to 1001 (design §8, decided
  2026-11-18).
- `Checkpoint::Load` hardened against a damaged file (the assessment's
  A8): the checksum stops accidents, not a file made to pass it.
- `fuzz.yaml` caller and one harness, for the database file, linked
  against the vendored engine without R, with a `reach-floor`. The
  harness recomputes the checksum of each input, so the fuzzer reaches
  `Checkpoint::Load` and is not stopped at the check.
- The Boundaries rows for the database file, and for each of the
  engine's file formats as refused.

**Tests.**

- Persistence: write, `shutdown`, reopen in a fresh R process
  (`callr`-free, through `Rscript`), read back `identical()`, in
  `test-persist.R`.
- The golden database file read back against its recorded rows, and a
  file of another format version a classed error naming both, in
  `test-persist.R` (Q7).
- Each `read_*` table function and each file extension as a bare path,
  for CSV, Parquet and JSON: `zuslothdb_sql_error` naming
  `zuslothdb_read()`, and the connection then answers a query, in
  `test-refused.R`.
- A database file with one bit flipped, at each of a spread of offsets
  from the header to the last byte, and one truncated at each of them:
  every one a classed error, none opens, and a new connection then
  works, in `test-hostile.R`; the large cases behind
  `ZUSLOTHDB_SKIP_HEAVY`.
- The fuzz harness: a file that opens gives the same rows on a second
  open, and each table's rows equal its `count(*)`, in `fuzz/`.
- An error thrown on a worker thread: `zuslothdb_sql_error` carrying the
  engine's message, and the connection then answers a query, in
  `test-conditions.R`. The stage finds a query that throws in a parallel
  operator; if none does once the readers are refused, the fork commit's
  own doctest case carries it and this line says so.
- No file written outside `tempdir()` and `dbdir`: a directory listing
  before and after the suite, in `test-hygiene.R`.

**Exit criteria.**

- A full-profile run is green on the pull request, `fuzz.yaml` included,
  with reach at or above the floor.
- Every `**Tests.**` line exists and was seen to fail.
- `tools/upstream-tests.sh` passes at the fork's new pin.
- Every crash the fuzzer found is fixed by a fork commit.

**Design.** [§7.5](design.md#75-the-engines-own-readers-q17),
[§8](design.md#8-persistence),
[§10](design.md#10-testing-and-validation) and
[Q7 and Q17](design.md#14-open-questions).

## Stage 7 — Arrow streams in

**Issue.** [#58](https://github.com/pedrobtz/zuslothdb/issues/58), sub-issue of
[v0.1.0](https://github.com/pedrobtz/zuslothdb/issues/1).

**CI.** `full — new glue that walks memory another library owns and
builds engine chunks from it: sanitizers, valgrind, rchk and step-20
gctorture.`

**Goal.** An Arrow C stream from any producer becomes a table, exactly
or with a classed error. This is the adapter the zu readers use in
Stage 10, built and tested before any of them has a release.

**Work.**

- As Q15 decided: the Arrow C stream is the one contract.
- `src/sdb_stream.cpp`: a stream's batches become `DataChunk`s and go in
  through `Connection::Append`. The two Arrow C struct definitions are
  copied from the specification; nothing is linked. The adapter pulls
  one chunk at a time, so Q19's scan operator can use it later.
- `dbWriteTableArrow()`, `dbAppendTableArrow()` and
  `dbCreateTableArrow()`. `nanoarrow` joins `Suggests` as the tests'
  producer.
- Types as design §6. A column of a type the engine cannot hold is
  refused from the stream's schema, before any row is appended.
- A load that fails part-way leaves nothing: a new table is dropped.
  How an append to an existing table stays whole without undo is
  settled here (a staging table is the proposal) and recorded in design
  §7.3.
- The Objects row for a stream (who releases it, and when), the
  Boundaries row for the Arrow C data interface, and the Limits rows for
  a batch longer than one chunk and one row group.

**Tests.**

- Each §6 type with NULLs through a `nanoarrow` stream: the table equals
  what `dbWriteTable()` makes of the same data frame, two routes to one
  value, in `test-arrow-stream.R`.
- Batches of 2047, 2048 and 2049 rows, and of 122,879, 122,880 and
  122,881, with a NULL on each side of the boundary (the class of #45):
  `identical()` to the input, in `test-arrow-stream.R`.
- A decimal, a time, a list, a struct and a dictionary column: each a
  `zuslothdb_type_error` naming the column, and no table made, in
  `test-arrow-stream.R`.
- A producer scripted to fail at its third batch: the producer's message
  in a classed error, no table and no row left, and the stream released
  once, with valgrind as the leak oracle, in `test-arrow-stream.R`.
- Stream sequences (a released stream, one consumed twice, a batch whose
  schema differs from the stream's): each a classed error, in
  `test-lifecycle.R`.

**Exit criteria.**

- A full-profile run and a dispatched step-20 gctorture run are green on
  the pull request.
- Every `**Tests.**` line exists and was seen to fail.

**Design.** [§7.3](design.md#73-the-contract-q15),
[§6](design.md#6-types) and [Q15](design.md#14-open-questions).

## Stage 8 — Core conformance

**Issue.** [#59](https://github.com/pedrobtz/zuslothdb/issues/59), sub-issue of
[v0.1.0](https://github.com/pedrobtz/zuslothdb/issues/1).

**CI.** `full — fork commits to the engine's core: every platform leg
and the sanitizers must see each.`

**Goal.** How much of the core's SQL is right is measured and written
down, and what is wrong fails with an error, not a wrong answer.

**Work.**

- `tools/sqllogictest.R` at a pinned corpus revision, behind
  `ZUSLOTHDB_SLOW_TESTS`. Each case is counted as passed, refused with an
  error, wrong, or a crash or hang, and the counts are committed.
- The bar, as Q18 decided: on the pinned subset no case is wrong, a
  crash or a hang. The pass rate is reported, not targeted. The subset
  and its revision are chosen here and recorded with the counts.
- Every wrong answer, crash and hang is filed as an issue. Each is fixed
  by a fork commit, turned into an error, or listed in
  `oracle/known-differences.md`.
- The recorded-answer corpus widened beyond scalars: joins, `GROUP BY`,
  window functions, subqueries, `ORDER BY` with `LIMIT`.
- The 31 chunks still at the default capacity audited (design §4.2,
  #45).
- A fuzz harness for SQL text, linked without R.

**Tests.**

- sqllogictest: the corpus's own expected results, by
  `tools/sqllogictest.R`; the committed counts against a fresh run, in
  `test-conformance.R` behind `ZUSLOTHDB_SLOW_TESTS`.
- The widened corpus: the recorded `duckdb` answers, in
  `test-oracle-sql.R`.
- The corpus on one thread and on two: the same result, in
  `test-threads.R`.
- Each audited chunk filled past 2048 rows: the recorded answer, in
  `test-chunks.R`.
- The SQL harness: the same result on one thread and on two, in `fuzz/`.

**Exit criteria.**

- Q18's bar holds: the committed counts show no wrong answer, crash or
  hang on the pinned subset. If it cannot hold in one pull request, the
  stage lands the runner and the counts, and each remaining case is an
  issue that blocks Stage 14.
- No known wrong answer is without an issue and a row in
  `known-differences.md`.
- A full-profile run is green on the pull request.
- Every `**Tests.**` line exists and was seen to fail.

**Design.** [§10](design.md#10-testing-and-validation),
[D1](design.md#12-decisions) and [Q18](design.md#14-open-questions).

## Stage 9 — DBItest conformance

**Issue.** [#9](https://github.com/pedrobtz/zuslothdb/issues/9), sub-issue of
[v0.1.0](https://github.com/pedrobtz/zuslothdb/issues/1).

**CI.** `quick`.

**Goal.** The driver passes the DBI specification's own tests, with
every skip named and explained.

**Work.**

- `DBItest` in `Suggests`; `tests/testthat/test-DBItest.R` with the
  driver, connection, result and SQL suites; the skip list with a reason
  per entry; fixes for what it finds.
- The Boundaries row for the DBI specification.

**Tests.**

- The DBI specification: `DBItest`, in `test-DBItest.R`.
- The skip list holds only parameter binding and Arrow results: a test
  that
  fails when a skipped `DBItest` case starts passing or a new one is
  added without a reason, in `test-DBItest.R`.
- The deepest nested expression the parser accepts and one level past
  it: an answer and a `zuslothdb_sql_error`, not a stack overflow, in
  `test-hostile.R`.

**Exit criteria.**

- `R CMD check` passes on the quick profile with `DBItest` installed,
  and on `nosuggests` without it.
- Every `**Tests.**` line exists and was seen to fail.

**Design.** [§10](design.md#10-testing-and-validation).

## Stage 10 — Readers from the zu family

**Issue.** [#60](https://github.com/pedrobtz/zuslothdb/issues/60), sub-issue of
[v0.1.0](https://github.com/pedrobtz/zuslothdb/issues/1).

**CI.** `full — static archives from other packages linked on every OS,
and a configure script: each platform leg, and the symbol and import
checks of tools/ci/platform.sh.`

**Blocked on.** A release of each package below with the header and
archive of [design §7.2](design.md#72-what-each-reader-provides), in the
order of [§7.8](design.md#78-what-each-package-releases-in-order):

1. [zukomp#132](https://github.com/pedrobtz/zukomp/issues/132);
2. [zuarrow#251](https://github.com/pedrobtz/zuarrow/issues/251);
3. [zucsv#23](https://github.com/pedrobtz/zucsv/issues/23) and
   [qio#71](https://github.com/pedrobtz/qio/issues/71);
4. [zujson#108](https://github.com/pedrobtz/zujson/issues/108).

**Goal.** `zuslothdb_read()` loads a CSV, JSON or Parquet file into a
table with the values a reference gives, a damaged file is an error
that leaves nothing behind, and another package can register a reader
for a format of its own.

**Work.**

- First, the cut Q17 decided is sized: every reader of the engine out
  of the fork, with zstd, snappy and fastpforlib. If it fits the stage,
  one fork commit makes it, `tools/vendor.yaml`'s `copy` list loses the
  three libraries, and `inst/COPYRIGHTS` and `Authors@R` lose their
  holders. If the whole does not fit, the engine's zstd and its Parquet
  reader are cut here all the same, since `libzukomp.a`'s zstd cannot
  be linked beside them; the rest moves after v0.1.0, and this roadmap
  records it.
- As Q20 decided: the codec table and the codecs come from
  `libzukomp.a`, fetched once at load and handed to each reader.
- `LinkingTo: zukomp, zuarrow, zucsv, zujson, qio` with version floors.
  `configure` and `configure.win` resolve the archives; `tools/vendor.R`
  writes the `Makevars` templates they fill.
- `zuslothdb_read(conn, name, path, format = NULL, columns = NULL)`, as
  Q16 decided: the reader's stream into Stage 7's adapter, and into a
  new table.
- `zuslothdb_info()` reports each reader's version.
- Registered readers, step A of
  [design §16.5](design.md#165-steps) (Q21 and Q22, decided
  2026-11-18):
  - `inst/include/zuslothdb.h`: plain C, no R header, no engine type,
    an ABI version, and the versioned struct a reader registers (its
    format name, its file extensions, and an open function that takes
    a path and options as strings and fills a stream);
  - an exported function an extension calls from `.onLoad` with an
    external pointer to that struct, and one registry for the process;
  - `zuslothdb_read(format = )` looks a format up there after the
    built-in ones, and `zuslothdb_info()` lists what is registered;
  - what unloading an extension's namespace does (refused, or the
    reader unregistered), settled here and recorded in design §16.4;
  - a fixture extension package under `tools/` and a `consumer.yaml`
    workflow, since this package is now a provider (family rule
    R10.4).
- The Objects row for a registered reader: who owns the struct, and
  what holds its code in memory.
- File fixtures written by `tools/oracle.R` with `duckdb` as the
  reference writer, small enough for the tarball, with their provenance,
  joining those Stage 6 kept.
- The Boundaries rows for each format, naming the recorded answers here
  and the reader's own oracle in its package.

**Tests.**

- Each format and each Parquet codec: the recorded `duckdb` answers, in
  `test-oracle-files.R`.
- The inputs the engine's readers got wrong. CSV with doubled quotes, a
  quoted newline and a value past row 100 that does not fit the first
  rows' type; JSON with nested values, a key first seen late and mixed
  types; Parquet `DECIMAL`, `TIME`, millisecond and nanosecond
  `TIMESTAMP`, `UUID`, unsigned and nested columns. Each is the recorded
  answer or a `zuslothdb_type_error` naming the column, never another
  value, in `test-read-types.R`.
- `zuslothdb_read()` against the sibling's R reader followed by
  `dbWriteTable()`: the same table, in `test-read.R`.
- `zuslothdb_read()`: each format by its extension, `format =` over it,
  and an unknown or missing extension, a glob, two paths and a remote
  path each a `zuslothdb_argument_error`; `columns =` gives the table
  those columns in that order, and an unknown one is an error; an
  existing `name` is an error and the table unchanged, and replaced with
  `overwrite = TRUE`, in `test-read.R` (Q16).
- If the engine's readers were cut: `tools/upstream-tests.sh` passes at
  the new pin, and no zstd, snappy or fastpforlib symbol of the engine's
  is in the shared object, by `nm` in `tools/ci/platform.sh`.
- Paths with spaces, quotes and non-ASCII characters: the same answer as
  the plain path, in `test-paths.R`.
- Truncated and bit-flipped files of each format, and invalid UTF-8 in
  CSV: a classed error, no table left, and the connection then loads an
  intact file, in `test-hostile.R`.
- A registered reader, from the fixture extension, for a format of its
  own: `zuslothdb_read()` gives the table `dbWriteTable()` makes of the
  same data, in `test-extension.R`.
- Registration refused: a format registered twice, a built-in format's
  name, a struct of another ABI version and a null open function are
  each a classed error and leave the registry as it was, in
  `test-extension.R`.
- A registered reader whose stream fails at its third batch: its
  message in a classed error and no table left, in `test-extension.R`.
- The extension's namespace unloaded: the outcome design §16.4 records,
  and then the format is unknown or still works, never a crash, in
  `test-extension.R`.
- The fixture extension builds with only `LinkingTo: zuslothdb` for the
  header, on each OS: `consumer.yaml`.
- With the archives linked, only `R_init_zuslothdb` is exported and no
  forbidden symbol is imported: `nm`, in `tools/ci/platform.sh`, once
  per OS.

**Exit criteria.**

- A full-profile run is green on the pull request.
- Every `**Tests.**` line exists and was seen to fail.
- The package installs from source on a machine with only the CRAN
  releases of the three readers.

**Design.** [§7](design.md#7-files),
[§16](design.md#16-extensions-through-r-packages),
[D9 and D11](design.md#12-decisions) and
[Q16, Q17 and Q20 to Q22](design.md#14-open-questions).

## Stage 11 — Documentation and measured claims

**Issue.** [#10](https://github.com/pedrobtz/zuslothdb/issues/10), sub-issue of
[v0.1.0](https://github.com/pedrobtz/zuslothdb/issues/1).

**CI.** `quick`.

**Goal.** A reader can tell what the package does, how far to trust the
engine, and how it differs from `duckdb`, from measured numbers.

**Work.**

- The Get started vignette evaluated, with its output checked.
- README: the engine commit, the patch list, the sqllogictest pass rate,
  the known differences from `duckdb`, that SlothDB is experimental, and
  that a database file is tied to the package version.
- The sqllogictest counts Stage 8 committed.
- Reference index grouped in `_pkgdown.yml`; every exported function
  with examples that run.

**Tests.**

- The vignette's output: the values shown are the recorded oracle
  answers for the same queries, compared in `test-vignette.R`.
- The README's pass rate and patch count: regenerated by
  `tools/sqllogictest.R` and `tools/vendor.yaml`, compared in
  `test-readme.R`, so the prose cannot drift from the numbers.

**Exit criteria.**

- `R CMD check` passes on the quick profile with the vignette built.
- Every `**Tests.**` line exists and was seen to fail.

**Design.** [§13](design.md#13-acceptance-criteria-for-v010).

## Stage 12 — Assurance

**Issue.** [#11](https://github.com/pedrobtz/zuslothdb/issues/11), sub-issue of
[v0.1.0](https://github.com/pedrobtz/zuslothdb/issues/1).

**CI.** `quick`.

**Goal.** No new features. `.agents/assurance.md` is complete and every
check it names has been read, not only run.

**Work.**

- Every required check wired or `n/a` with a reason: `mutation` is
  expected to be `n/a`, since the package's own compiled code is glue
  with no R-free tests and the vendored engine is not the package's to
  mutate; confirm or wire it.
- Every Boundaries, Objects and Limits row filled.
- The fuzz-reach and native-coverage gap lists and the `alloc-failure`
  results read; each missing oracle filed as an issue.

**Tests.**

- No new behaviour. Each row of `.agents/assurance.md` names an existing
  test, checked by `assurance.yml`.

**Exit criteria.**

- `assurance.yaml` is green.
- Every remaining `gap #<issue>` is closed or moved under "After 0.1.0"
  with a reason.

**Design.** [§10](design.md#10-testing-and-validation).

## Stage 13 — Full CI

**Issue.** [#12](https://github.com/pedrobtz/zuslothdb/issues/12), sub-issue of
[v0.1.0](https://github.com/pedrobtz/zuslothdb/issues/1).

**CI.** `full`.

**Goal.** No new features. Everything the full r-actions profile finds
on the default branch's code is fixed.

**Work.**

- Run the full profile under `full-ci`: valgrind, ASan, every platform
  leg, `nosuggests`, `nold`, cran-special. Dispatch gctorture at step
  20, and fix what it and any open `gctorture` issue report.

**Tests.**

- No new behaviour; a fix that changes behaviour adds its test beside
  the stage that owns the code.

**Exit criteria.**

- A full-profile run green on the pull request, a dispatched one green
  on the merge commit, and a dispatched step-20 gctorture run green on
  the pull request's last commit.

**Design.** [§13](design.md#13-acceptance-criteria-for-v010).

## Stage 14 — v0.1.0 readiness

**Issue.** [#13](https://github.com/pedrobtz/zuslothdb/issues/13), sub-issue of
[v0.1.0](https://github.com/pedrobtz/zuslothdb/issues/1).

**CI.** `full`.

**Goal.** The tree is what will be submitted.

**Work.**

- `Version: 0.1.0` and the matching `NEWS.md` heading.
- `cran-comments.md` listing the r-actions CI legs as test environments
  and explaining any size NOTE.
- `urlchecker::url_check()` and `spelling::spell_check_package()` clean.

**Tests.**

- No new behaviour.

**Exit criteria.**

- A full-profile r-actions run is green on the merge commit.
- Every item of [§13](design.md#13-acceptance-criteria-for-v010) holds.

**Design.** [§13](design.md#13-acceptance-criteria-for-v010).

## Stage 15 — Review

**Issue.** [#14](https://github.com/pedrobtz/zuslothdb/issues/14), sub-issue of
[v0.1.0](https://github.com/pedrobtz/zuslothdb/issues/1).

**CI.** `full`.

**Goal.** No new features. The release candidate has been reviewed
twice and what the reviews required is fixed.

**Work.**

- Critical review with `pkg-critical-review`; its blocking and required
  findings become sub-issues of this stage's issue and are fixed in one
  pull request.
- CRAN review with `pkg-cran-review`, after those fixes.

**Tests.**

- Each fixed finding adds the test that would have caught it.

**Exit criteria.**

- Every blocking and required review issue is closed, the
  `pkg-cran-review` table is clean, and `NEWS.md` reflects any
  user-visible fix.
- A full-profile run is green on the default branch, whose head becomes
  the release commit.

**Design.** All of design.md.

## After 0.1.0

- [§2](design.md#2-scope): parameter binding (Q3), Arrow export through
  `nanoarrow`, ALTREP results, R functions as SQL functions and `dbplyr`
  translations (Q5) wait, as the design says.
- [§16.5](design.md#165-steps), extensions through R packages (D11),
  one release each, in the order Q21 decided: B,
  table functions in SQL over a stream (Q19), which brings back
  `SELECT ... FROM read_csv('x')`; C, registered scalar functions;
  D, results out as Arrow streams.
- [§7.1](design.md#71-the-readers): Excel through `zuxlsx` and Arrow IPC
  through `zuarrow`
  ([zuarrow#251](https://github.com/pedrobtz/zuarrow/issues/251)), once
  each has a release with its header and archive.
- [§5.1](design.md#51-the-api-against-its-two-references):
  `duckdb_register()`'s counterpart needs a scan over R memory in the
  engine, which Q19's table function would give.
- [Q4](design.md#14-open-questions): `memory_limit` needs engine work.
- [Q10](design.md#14-open-questions): `zuslothdb_sql()` waits unless it is
  adopted before Stage 10, which would then carry it.

## Blocking open questions

- None. Q15 to Q23, and the database file's checksum, were decided on
  2026-11-18 ([design §14](design.md#14-open-questions) and
  [§8](design.md#8-persistence)).
- Still to find, inside a stage: how an append to an existing table
  stays whole without undo (Stage 7); how large the cut of the engine's
  readers is, and what unloading an extension does (Stage 10).
