# zuslothdb: roadmap

Status: draft 2026-11-17. Implements [design.md](design.md).
Target: v0.1.0, the first CRAN release.
Tracking: [v0.1.0 (#1)](https://github.com/pedrobtz/zuslothdb/issues/1),
with one sub-issue per stage;
[CRAN-submission (#15)](https://github.com/pedrobtz/zuslothdb/issues/15).

Stage 1 is a go/no-go spike. If its numbers meet the stop condition of
[D1](design.md#12-decisions), the package stops there: the reason is
recorded in the design and the remaining stage issues are closed as not
planned.

## Stages

<!-- markdownlint-disable MD013 -->
| Stage | Title | CI | Issue | Pull request | Done |
| --- | --- | --- | --- | --- | --- |
| 0 | [Package identity](#stage-0--package-identity) | quick | [#2](https://github.com/pedrobtz/zuslothdb/issues/2) | [#27](https://github.com/pedrobtz/zuslothdb/pull/27) | 2026-11-17 |
| 1 | [Engine spike](#stage-1--engine-spike) | full | [#3](https://github.com/pedrobtz/zuslothdb/issues/3) | [#28](https://github.com/pedrobtz/zuslothdb/pull/28) | 2026-11-17 |
| 2 | [Driver and connection](#stage-2--driver-and-connection) | full | [#4](https://github.com/pedrobtz/zuslothdb/issues/4) | [#30](https://github.com/pedrobtz/zuslothdb/pull/30) | 2026-11-17 |
| 3 | [Results and scalar types](#stage-3--results-and-scalar-types) | full | [#5](https://github.com/pedrobtz/zuslothdb/issues/5) | [#31](https://github.com/pedrobtz/zuslothdb/pull/31) | 2026-11-17 |
| 4 | [Temporal, decimal and nested types](#stage-4--temporal-decimal-and-nested-types) | quick | [#6](https://github.com/pedrobtz/zuslothdb/issues/6) | | |
| 5 | [Writing and transactions](#stage-5--writing-and-transactions) | quick | [#7](https://github.com/pedrobtz/zuslothdb/issues/7) | | |
| 6 | [Files and persistence](#stage-6--files-and-persistence) | full | [#8](https://github.com/pedrobtz/zuslothdb/issues/8) | | |
| 7 | [DBItest conformance](#stage-7--dbitest-conformance) | quick | [#9](https://github.com/pedrobtz/zuslothdb/issues/9) | | |
| 8 | [Documentation and measured claims](#stage-8--documentation-and-measured-claims) | quick | [#10](https://github.com/pedrobtz/zuslothdb/issues/10) | | |
| 9 | [Assurance](#stage-9--assurance) | quick | [#11](https://github.com/pedrobtz/zuslothdb/issues/11) | | |
| 10 | [Full CI](#stage-10--full-ci) | full | [#12](https://github.com/pedrobtz/zuslothdb/issues/12) | | |
| 11 | [v0.1.0 readiness](#stage-11--v010-readiness) | full | [#13](https://github.com/pedrobtz/zuslothdb/issues/13) | | |
| 12 | [Review](#stage-12--review) | full | [#14](https://github.com/pedrobtz/zuslothdb/issues/14) | | |
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

**Work.**

- `DATE`, `TIME`, `TIMESTAMP`, `TIMESTAMP_TZ` with `timezone_out`;
  `DECIMAL` with `decimal`; `BLOB`; `LIST`; `STRUCT`; the errors for
  `MAP`, `UNION` and `HUGEINT`. Q1 read and recorded.

**Tests.**

- Each type, with NULLs at every nesting level and empty lists: the
  recorded `duckdb` answers, in `test-oracle-types.R`.
- Dates and timestamps before 1970, on leap days and at the type's
  extremes: the recorded answers, in `test-temporal.R`.
- `DECIMAL` at the largest precision under both `decimal` values: the
  literal's decimal string, in `test-decimal.R`.
- `MAP`, `UNION`, `HUGEINT`: `zuslothdb_type_error` naming the column, in
  `test-conditions.R`.

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
- Rollback: the table equals its state before `dbBegin()`; an error
  mid-append leaves no partial rows, in `test-transactions.R`.
- Transaction sequences (commit without begin, begin twice, disconnect
  inside a transaction): in `test-lifecycle.R`.

**Exit criteria.**

- `R CMD check` passes on the quick profile.
- Every `**Tests.**` line exists and was seen to fail.

**Design.** [§5](design.md#5-public-r-api-proposed) and
[§6](design.md#6-types).

## Stage 6 — Files and persistence

**Issue.** [#8](https://github.com/pedrobtz/zuslothdb/issues/8), sub-issue of
[v0.1.0](https://github.com/pedrobtz/zuslothdb/issues/1).

**CI.** `full — the file readers and the database file parse bytes the
package did not write: ASan, valgrind and the fuzz harnesses.`

**Goal.** SQL reads CSV, Parquet, JSON and Arrow IPC files with the
answers a reference gives, a database file survives a restart, and
damaged input is an error the session survives.

**Work.**

- `zuslothdb_read()`, with Q12 decided; Q7 (refusing a file from another
  engine commit) decided.
- File fixtures written by `tools/oracle.R` with `duckdb` as the
  reference writer, small enough for the tarball, with their provenance.
- `fuzz.yaml` caller and one harness per compiled reader, linked against
  the vendored engine without R, with a `reach-floor`.
- The Boundaries rows for each file format and the database file.

**Tests.**

- Each format, each Parquet codec (gzip, zstd, snappy) and an
  unsupported codec: the recorded `duckdb` answers, or
  `zuslothdb_sql_error`, in `test-oracle-files.R`. The formats include
  Avro, Excel and SQLite, which Q11 compiled in (2026-11-17), with a
  reference reader's recorded answers for each.
- Paths with spaces, quotes and non-ASCII characters: the same answer as
  the plain path, in `test-paths.R`.
- Persistence: write, `shutdown`, reopen in a fresh R process
  (`callr`-free, through `Rscript`), read back `identical()`, in
  `test-persist.R`.
- Truncated and bit-flipped files of each format, invalid UTF-8 in CSV,
  a corrupted database file: `zuslothdb_sql_error`, and the same
  connection then returns the recorded answer for an intact file, in
  `test-hostile.R`; the large cases behind `ZUSLOTHDB_SKIP_HEAVY`.
- Each fuzz harness: rows returned equal `count(*)` over the same bytes
  and a second read is identical, in `fuzz/`.
- No file written outside `tempdir()` and `dbdir`: a directory listing
  before and after the suite, in `test-hygiene.R`.

**Exit criteria.**

- A full-profile run is green on the pull request, `fuzz.yaml` included,
  with reach at or above the floor.
- Every `**Tests.**` line exists and was seen to fail.
- Every crash the fuzzers found is patched in `tools/vendor.yaml` or the
  reader is compiled out; the patch total is still within D1.

**Design.** [§7](design.md#7-files), [§8](design.md#8-persistence) and
[§10](design.md#10-testing-and-validation).

## Stage 7 — DBItest conformance

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
- The skip list holds only parameter binding and Arrow: a test that
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

## Stage 8 — Documentation and measured claims

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
- `tools/sqllogictest.R` behind `ZUSLOTHDB_SLOW_TESTS`, at a pinned corpus
  revision.
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

## Stage 9 — Assurance

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

## Stage 10 — Full CI

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

## Stage 11 — v0.1.0 readiness

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

## Stage 12 — Review

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
- [§5.1](design.md#51-the-api-against-its-two-references):
  `duckdb_register()`'s counterpart needs a scan over R memory in the
  engine.
- [Q4](design.md#14-open-questions): `memory_limit` needs engine work.
- [Q10](design.md#14-open-questions): `zuslothdb_sql()` waits unless it is
  adopted before Stage 6, which would then carry it.

## Blocking open questions

- [Q1](design.md#14-open-questions) blocks Stage 4.
- [Q7, Q12](design.md#14-open-questions) block Stage 6.
