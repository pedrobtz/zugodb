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

`zugodb` (named `zuslothdb` until 2026-11-18, design D12) is to
[libzugodb](https://github.com/pedrobtz/libzugodb), its own C++20 engine
derived from SlothDB, what `duckdb` is to DuckDB: the engine's
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
§16). The engine's core is SlothDB's, now maintained as libzugodb.

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

Stages 0 to 7 are merged: the patched engine, the DBI driver and
connection, results and scalar types, `DATE`, `TIMESTAMP` and `BLOB`,
writing, the engine as libzugodb and the package as zugodb, (Stage 6)
libzugodb 0.4.0: format 1001 with a checksum, no file read from SQL, and
the engine's readers and their codecs gone, and (Stage 7) Arrow C streams
in through `dbWriteTableArrow()`, the adapter the readers use later. The
roadmap in [.agents/roadmap.md](.agents/roadmap.md) was restaged on
2026-11-18 from Stage 6 on:

- Stage 8: the core measured against sqllogictest. The bar for v0.1.0 is no
  wrong answer, crash or hang on a pinned subset (Q18).
- Stage 10: the zu readers linked, `zugodb_read()`, readers registered
  by other packages, and the engine's readers cut.

v0.1.0 has no CRAN date. D1 in the design records why the package goes on.

Next: Stage 8, core conformance.

## Versions and dependents

The current version's parent issue is
[#1](https://github.com/pedrobtz/zugodb/issues/1) for `v0.1.0`.
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

`src/Makevars`, `src/Makevars.win`, `src/zug_provenance.h`,
`inst/vendor.yaml` and `tools/vendor/checksums.md5` are written by
`tools/vendor.R`: change `tools/vendor.yaml` and re-run it,
never edit them. Set `MAKEFLAGS=-jN` for any local build; the engine is
about four CPU-minutes serial. `ZUG_TEST_SEED` replays a shuffled
test order.

## Architecture

Three layers ([.agents/design.md](.agents/design.md) §4):

- `src/vendor/libzugodb/`: the engine, a tagged release of
  [pedrobtz/libzugodb](https://github.com/pedrobtz/libzugodb).
- [src/zug_glue.cpp](src/zug_glue.cpp): every `.Call` entry point and
  `R_init_zugodb`. Engine work happens in `Engine*` helpers inside
  `try`/`catch` with all C++ objects scoped there; entry points get back
  plain values and an error string, then build R objects.
- [src/zug_database.cpp](src/zug_database.cpp): databases and connections
  behind external pointers. A connection's `prot` is its database's
  pointer, so the database struct outlives it; closing a database
  destroys its connections' engine objects first. Every struct records
  its process; from another (a fork) it is closed and its finalizer
  does nothing.
- [src/zug_result.cpp](src/zug_result.cpp): results in two phases. The
  C++ phase copies the engine's whole result into plain buffers (and
  finds any value R cannot hold); the R phase only slices those buffers
  into vectors, so no C++ object is live while R allocates.
- [src/zug_stream.cpp](src/zug_stream.cpp): an Arrow C stream into a
  table ([src/zug_arrow.h](src/zug_arrow.h) has the specification's
  structs). A load moves the stream into a holder that releases it once;
  the producer's callbacks run in the `.Call` frame, each batch is
  converted inside `try`/`catch`. It also holds the tests' scripted
  producer, `zug_scripted_stream()`, which counts its releases.
- `R/`: [R/result.R](R/result.R) (the `zugodb_result` methods),
  [R/arrow.R](R/arrow.R) (the Arrow methods: a new table dropped on
  failure, an append or overwrite through a staging table),
  [R/driver.R](R/driver.R) (`zugodb()`, the database it holds,
  opened at the first connection), [R/connection.R](R/connection.R)
  (`dbConnect()` to `dbExecute()`), [R/conditions.R](R/conditions.R)
  (`zug_abort()` turns the glue's error kind into a condition class),
  [R/checks.R](R/checks.R) (argument checks), [R/info.R](R/info.R).

## Invariants that are easy to break

- No C++ exception crosses into R, and no R API call that can longjmp runs
  while an engine object with a destructor is live on the stack (§4.3).
  Breaking either leaks or crashes the session. The glue never calls
  `Rf_error()`; keep it that way.
- The shared library exports only `R_init_zugodb` (plus the C++
  standard library's own template typeinfo), so no engine symbol binds
  to another package's (it carried DuckDB's `duckdb_fastpforlib` and
  zstd until libzugodb 0.4.0). `platform.sh` checks it on each OS, and
  `tools/ci/coexist.R` runs both packages in one session.
- Nothing vendored is edited by hand: an engine change is a libzugodb
  release, vendored by tag (D12). `tools/vendor/verify` and the vendor
  workflow fail otherwise.
- No `stderr`, `stdout`, `printf`, `abort`, `exit` or `rand` linked:
  `platform.sh` checks the shared object's imports.
- Offline only (D4), and no file written outside `tempdir()` and the
  user's `dbdir`: the fork's "Refuse remote paths" change makes a remote
  path an error.
- Two threads unless the user sets `threads` (D5): the fork's
  process-wide cap, `slothdb::SetThreadCap()`.
- A 64-bit integer that does not fit a double is a `zug_type_error`,
  never a silent rounding (§6).

## Testing

`tests/testthat/` runs shuffled with the seed printed
([tests/testthat.R](tests/testthat.R), `tools/ci-run-tests.R`). So far:
`test-spike.R` (the Stage 1 query path), `test-info.R` (the compiled-in
provenance against the installed `vendor.yaml`), `test-lifecycle.R`
(out-of-order sequences), `test-connect.R` (arguments, read-only,
threads), `test-fork.R`, `test-unwind.R` (valgrind is its leak oracle)
and `test-conditions.R`; Stage 3's `test-oracle-scalar.R`, `test-fetch.R`,
`test-bigint.R` and `test-varchar.R`; Stage 6's `test-persist.R` (a
fresh R process, and the golden file `fixtures/golden/`, written by
`tools/golden.R` only with a new format version), `test-hostile.R`,
`test-refused.R` and `test-hygiene.R`; Stage 7's `test-arrow-stream.R`
(with `zuarrow`, and the package's own producers for what no
producer should make). Recorded answers: run
`Rscript tools/oracle.R [name]` (needs duckdb) after adding or changing a
`tests/testthat/oracle/*.sql`; a disagreement is either fixed or listed
in `oracle/known-differences.md`, whose rows the tests hold to "still
differs". Helpers live in `helper-*.R`: testthat's shuffled run skips
file-scope code. A finalizer's warning
cannot be caught around `gc()` (R runs finalizers through
`R_ToplevelExec`): test the effect of collection, and call the finalizer
for the warning. Upstream's own doctest suite runs
on the vendored tree through `tools/upstream-tests.sh` (491 cases), and
`tools/ci/coexist.R` runs `duckdb` in the same session: both from
`tools/ci/platform.sh`, never from testthat, because §10 keeps `duckdb`
out of `Suggests`. The design's §10 plans DuckDB's recorded answers as the
oracle, `DBItest`, and hostile inputs under the sanitizers, with
`ZUG_SKIP_HEAVY` and `ZUG_SLOW_TESTS` as the switches.

## Continuous integration

All through `pedrobtz/r-actions` `@v1`, on this package's Saturday 07:00
UTC slot: `R-CMD-check.yaml` (plus Windows R-devel in the full profile),
`coverage.yaml` (pinned to a commit; `native: true`), `pkgdown.yaml`,
`assurance.yaml` (`ungated-jobs: fail`), `platforms.yaml` (runs
`tools/ci/platform.sh` on macOS, Windows and Ubuntu), `native-checks.yaml`
(sanitizers, valgrind, LTO, rchk, analyzers, cran-special),
`gctorture.yaml` (wide step on pull requests, step 20 daily), `vendor.yaml`
and `vendor-upstream.yaml`. The roadmap marks Stages 1, 2, 3, 5.1, 5.2,
6, 7, 8 and 10 `full`, besides the three closing stages.

## Vendored native code

The engine is [libzugodb](https://github.com/pedrobtz/libzugodb) (MIT),
this package's own C++ engine since 2026-11-18 (design D12): SlothDB at
upstream `8180f215847015944c935f56f008d194e2c1494c` ("v0.2.7+"), by the
SlothDB Contributors, with the changes in its `CHANGELOG.md`, under the
C++ namespace `zugodb::`. A tagged release is vendored in
`src/vendor/libzugodb/`: `include/`, `src/` and `LICENSE`, with
`unordered_dense.h` under `include/third_party/` its only third-party
code (zstd, snappy, fastpforlib and miniz went with the file readers in
libzugodb 0.4.0). Left out: the CMake files. Holders are in `inst/COPYRIGHTS`;
[.agents/VENDORED.md](.agents/VENDORED.md) has the details and the
upstream reports. To change the engine: a pull request to libzugodb, whose
CI runs its doctests on Linux (with ASan and UBSan), macOS and MinGW-w64;
a release tag; then set `source.tag` and `source.commit` in
`tools/vendor.yaml`, run `Rscript tools/vendor.R` and
`tools/upstream-tests.sh`.
