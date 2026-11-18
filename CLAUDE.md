# CLAUDE.md

<!-- markdownlint-disable-next-line MD013 -->
This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Shared conventions

<!-- r-skills: shared-rules -->

The portfolio's shared rules (`workflow`, `r-package`, `ci`, `native-code`,
`correctness`, `assurance`) come from the
[r-skills](https://github.com/pedrobtz/r-skills) plugin's `rules/`, loaded at
session start because of the marker above. Never copy them here. Where this
file and a shared rule disagree, this file wins for this package. Cloud
sessions do not load the plugin and run without them.

## What this is

`zuslothdb` is to SlothDB what `duckdb` is to DuckDB: the engine's C++20
source vendored into the package and compiled at install, with a DBI driver
on top, so that SQL over local CSV, Parquet and JSON files works
with no system library, no server and no download. Runtime dependencies are
`DBI` and `methods` only.

Since 2026-11-18 (design D9 to D11) it aims to be a small engine that other
R packages extend. Files are read by the zu family's readers (`zucsv`,
`zujson`, `qio`), linked at build time as static archives over one contract,
the Arrow C stream, and not by SlothDB's own readers, which are refused and
then cut (design §7). The engine loads no code itself: an extension is an R
package that registers table sources and scalar functions with it (design
§16). The engine's core stays SlothDB's and is fixed on the fork.

It is deliberately not a `duckdb` replacement
(upstream calls itself experimental), not a network client (HTTP, S3,
extension loading and the `ask` module are compiled out), and not a `dplyr`
backend of its own in v0.1.0.

[.agents/design.md](.agents/design.md) is the specification and outranks
this file; it records the RFC it came from. Its §5.1 fixes the rule for the
API: the shape is `duckdb`'s, the scope is upstream's Python binding's. Its
§12 decisions and §14 open questions are the maintainer's to settle; do not
settle one in code without recording it there. D10 is the rule for a
conflict with an earlier design, here or in a sibling: the design is
rewritten to fit, provided what already works does not change.
[.agents/slothdb-assessment.md](.agents/slothdb-assessment.md) is the
evidence behind D9: what testing found wrong in the engine and its readers.

## Current state

Stages 0 to 5 are merged: the patched engine, the DBI driver and
connection, results and scalar types, `DATE`, `TIMESTAMP` and `BLOB`, and
writing. The roadmap in [.agents/roadmap.md](.agents/roadmap.md) was
restaged on 2026-11-18 from Stage 6 on:

- Stage 6: persistence, the database file's checksum, and the engine's own
  readers refused.
- Stage 7: Arrow C streams in, through DBI's `dbWriteTableArrow()`; the
  adapter the readers use later.
- Stage 8: the core measured against sqllogictest. The bar for v0.1.0 is no
  wrong answer, crash or hang on a pinned subset (Q18).
- Stage 10: the zu readers linked, `zuslothdb_read()`, readers registered
  by other packages, and the engine's readers cut.

v0.1.0 has no CRAN date. D1 in the design records why the package goes on.

## Versions and dependents

The current version's parent issue is
[#1](https://github.com/pedrobtz/zuslothdb/issues/1) for `v0.1.0`.
Sibling packages that depend on this one before CRAN does: none.

It waits on a release of each of `zukomp`, `zuarrow`, `zucsv`, `qio` and
`zujson` with a C header and a static archive. Design §7.8 has their
issues, in the order they link; Stage 7 needs none of them.

## Commands

```sh
Rscript tools/vendor.R                    # re-vendor from tools/vendor.yaml
Rscript tools/vendor.R --source <clone>   # the same from a local clone
tools/vendor/verify                       # rebuild in tempdir and compare
MAKEFLAGS=-j8 R CMD INSTALL . && Rscript tools/ci-run-tests.R
sh tools/upstream-tests.sh [workdir]      # the fork's doctests, vendored tree
bash tools/ci/platform.sh <macos|windows|ubuntu>  # the per-OS checks
```

`src/Makevars`, `src/Makevars.win`, `src/sdb_provenance.h`,
`inst/vendor.yaml` and `tools/vendor/checksums.md5` are written by
`tools/vendor.R`: change `tools/vendor.yaml` and re-run it,
never edit them. Set `MAKEFLAGS=-jN` for any local build; the engine is
about four CPU-minutes serial. `ZUSLOTHDB_TEST_SEED` replays a shuffled
test order.

## Architecture

Three layers ([.agents/design.md](.agents/design.md) §4):

- `src/vendor/slothdb/`: the engine, from the fork pedrobtz/slothdb.
- [src/sdb_glue.cpp](src/sdb_glue.cpp): every `.Call` entry point and
  `R_init_zuslothdb`. Engine work happens in `Engine*` helpers inside
  `try`/`catch` with all C++ objects scoped there; entry points get back
  plain values and an error string, then build R objects.
- [src/sdb_database.cpp](src/sdb_database.cpp): databases and connections
  behind external pointers. A connection's `prot` is its database's
  pointer, so the database struct outlives it; closing a database
  destroys its connections' engine objects first. Every struct records
  its process; from another (a fork) it is closed and its finalizer
  does nothing.
- [src/sdb_result.cpp](src/sdb_result.cpp): results in two phases. The
  C++ phase copies the engine's whole result into plain buffers (and
  finds any value R cannot hold); the R phase only slices those buffers
  into vectors, so no C++ object is live while R allocates.
- `R/`: [R/result.R](R/result.R) (the `zuslothdb_result` methods),
  [R/driver.R](R/driver.R) (`zuslothdb()`, the database it holds,
  opened at the first connection), [R/connection.R](R/connection.R)
  (`dbConnect()` to `dbExecute()`), [R/conditions.R](R/conditions.R)
  (`sdb_abort()` turns the glue's error kind into a condition class),
  [R/checks.R](R/checks.R) (argument checks), [R/info.R](R/info.R).

## Invariants that are easy to break

- No C++ exception crosses into R, and no R API call that can longjmp runs
  while a SlothDB object with a destructor is live on the stack (§4.3).
  Breaking either leaks or crashes the session. The glue never calls
  `Rf_error()`; keep it that way.
- The shared library exports only `R_init_zuslothdb` (plus the C++
  standard library's own template typeinfo). SlothDB carries a
  `duckdb_fastpforlib` namespace copied from DuckDB, and zstd, which
  other packages bundle too. `platform.sh` checks it on each OS, and
  `tools/ci/coexist.R` runs both packages in one session.
- Nothing vendored is edited by hand: every change to upstream is a
  commit on the fork's `zuslothdb` branch, listed in `tools/vendor.yaml`'s
  `changes` with its reason (§4.2, D3). `tools/vendor/verify` and the
  vendor workflow fail otherwise.
- No `stderr`, `stdout`, `printf`, `abort`, `exit` or `rand` linked:
  `platform.sh` checks the shared object's imports.
- Offline only (D4), and no file written outside `tempdir()` and the
  user's `dbdir`: the fork's "Refuse remote paths" change makes a remote
  path an error.
- Two threads unless the user sets `threads` (D5): the fork's
  process-wide cap, `slothdb::SetThreadCap()`.
- A 64-bit integer that does not fit a double is a `zuslothdb_type_error`,
  never a silent rounding (§6).

## Testing

`tests/testthat/` runs shuffled with the seed printed
([tests/testthat.R](tests/testthat.R), `tools/ci-run-tests.R`). So far:
`test-spike.R` (the Stage 1 query path), `test-info.R` (the compiled-in
provenance against the installed `vendor.yaml`), `test-lifecycle.R`
(out-of-order sequences), `test-connect.R` (arguments, read-only,
threads), `test-fork.R`, `test-unwind.R` (valgrind is its leak oracle)
and `test-conditions.R`; Stage 3's `test-oracle-scalar.R`, `test-fetch.R`,
`test-bigint.R` and `test-varchar.R`. Recorded answers: run
`Rscript tools/oracle.R [name]` (needs duckdb) after adding or changing a
`tests/testthat/oracle/*.sql`; a disagreement is either fixed or listed
in `oracle/known-differences.md`, whose rows the tests hold to "still
differs". Helpers live in `helper-*.R`: testthat's shuffled run skips
file-scope code. A finalizer's warning
cannot be caught around `gc()` (R runs finalizers through
`R_ToplevelExec`): test the effect of collection, and call the finalizer
for the warning. Upstream's own doctest suite runs
on the vendored tree through `tools/upstream-tests.sh` (378 cases), and
`tools/ci/coexist.R` runs `duckdb` in the same session: both from
`tools/ci/platform.sh`, never from testthat, because §10 keeps `duckdb`
out of `Suggests`. The design's §10 plans DuckDB's recorded answers as the
oracle, `DBItest`, and hostile inputs under the sanitizers, with
`ZUSLOTHDB_SKIP_HEAVY` and `ZUSLOTHDB_SLOW_TESTS` as the switches.

## Continuous integration

All through `pedrobtz/r-actions` `@v1`, on this package's Saturday 07:00
UTC slot: `R-CMD-check.yaml` (plus Windows R-devel in the full profile),
`coverage.yaml` (pinned to a commit; `native: true`), `pkgdown.yaml`,
`assurance.yaml` (`ungated-jobs: fail`), `platforms.yaml` (runs
`tools/ci/platform.sh` on macOS, Windows and Ubuntu), `native-checks.yaml`
(sanitizers, valgrind, LTO, rchk, analyzers, cran-special),
`gctorture.yaml` (wide step on pull requests, step 20 daily), `vendor.yaml`
and `vendor-upstream.yaml`. The roadmap marks Stages 1, 2, 3, 6, 7, 8
and 10 `full`, besides the three closing stages.

## Vendored native code

[SlothDB](https://github.com/SouravRoy-ETL/slothdb) (MIT), from the
fork [pedrobtz/slothdb](https://github.com/pedrobtz/slothdb)'s
`zuslothdb` branch: upstream `8180f215847015944c935f56f008d194e2c1494c`
("v0.2.7+") plus zuslothdb's changes, one commit each (D3), in
`src/vendor/slothdb/`, with its `third_party/` subset: the zstd
decompressor, snappy, fastpforlib and, inside `src/storage/`, miniz;
`unordered_dense.h` under `include/third_party/`. Left out: `src/ask/`,
`src/extension/`, `src/api/` (the C API), the HTTP client and upstream's
CMake files. [.agents/VENDORED.md](.agents/VENDORED.md) has the fork's
branches, the patch ledger and how a change is offered upstream. The
changes are listed in `tools/vendor.yaml`'s `changes`
with their reasons; holders in `inst/COPYRIGHTS`. To change the engine:
commit to the fork's `zuslothdb` branch (one change per commit, its
reason in the message, the zuslothdb issue as `pedrobtz/zuslothdb#n`),
tag it `zuslothdb-<n+1>`, push both, add the commit to `changes` and set
`source.tag` and `source.commit`, then run `Rscript tools/vendor.R` and
`tools/upstream-tests.sh`. Should upstream move, rebase the branch onto
it and update `upstream.commit` (and `describe`) too.
