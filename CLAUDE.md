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
on top, so that SQL over local CSV, Parquet, JSON and Arrow IPC files works
with no system library, no server and no download. Runtime dependencies are
`DBI` and `methods` only. It is deliberately not a `duckdb` replacement
(upstream calls itself experimental), not a network client (HTTP, S3,
extension loading and the `ask` module are compiled out), and not a `dplyr`
backend of its own in v0.1.0.

[.agents/design.md](.agents/design.md) is the specification and outranks
this file; it records the RFC it came from. Its §5.1 fixes the rule for the
API: the shape is `duckdb`'s, the scope is upstream's Python binding's. Its
§12 decisions and §14 open questions are the maintainer's to settle; do not
settle one in code without recording it there.

## Current state

Scaffolded 2026-11-17 with `create-pkg.sh`; no functionality yet, and
`DESCRIPTION` still has the scaffold's placeholder title, description and
author. Nothing of the engine has been built with any compiler. The roadmap
in [.agents/roadmap.md](.agents/roadmap.md) takes it to v0.1.0. Its Stage 1
is a go/no-go spike (decision D1): the package may stop there.

## Versions and dependents

The current version's parent issue is
[#1](https://github.com/pedrobtz/zuslothdb/issues/1) for `v0.1.0`.
Sibling packages that depend on this one before CRAN does: none.

## Commands

Only the shared commands so far. The design plans `tools/vendor.R`, which
copies the pinned upstream commit, applies the patches and writes the
source list into `src/Makevars`.

## Architecture

To be written as stages land; see [.agents/design.md](.agents/design.md) §4
for the three planned layers (vendored C++, `sdb_` glue C++, R DBI classes).
The scaffold set up a C stub, [src/zuslothdb.c](src/zuslothdb.c), through
`use_c()`; the design needs C++20 (`CXX_STD = CXX20`), so Stage 1 replaces
it.

## Invariants that are easy to break

None is enforced by code yet. The design already fixes these:

- No C++ exception crosses into R, and no R API call that can longjmp runs
  while a SlothDB object with a destructor is live on the stack (§4.3).
  Breaking either leaks or crashes the session.
- The shared library exports only `R_init_zuslothdb`. SlothDB carries a
  `duckdb_fastpforlib` namespace copied from DuckDB, so visible symbols
  clash when `duckdb` is loaded in the same session.
- Nothing vendored is edited by hand: every change to upstream is a patch
  recorded in `tools/vendor.yaml` with its reason (§4.2).
- Offline only (D4), and no file written outside `tempdir()` and the
  user's `dbdir`.
- Two threads unless the user sets `threads` (D5).
- A 64-bit integer that does not fit a double is a `zuslothdb_type_error`,
  never a silent rounding (§6).

## Testing

None yet: no conformance suite, helpers or measured runtime. The design's
§10 plans DuckDB as the oracle (in a separate development project, not in
`Suggests`), `DBItest`, upstream's doctest suite on the patched tree, and
hostile inputs under the sanitizers, with `ZUZUSLOTHDB_SKIP_HEAVY` and
`ZUZUSLOTHDB_SLOW_TESTS` as the switches.

## Continuous integration

`R-CMD-check.yaml` and `coverage.yaml` call the shared
`pedrobtz/r-actions` workflows (`r-cmd-check.yml@v1`, `coverage.yml@v1`).
`pkgdown.yaml` is the r-lib example workflow, building the site to
`gh-pages`. The roadmap marks Stages 1, 2, 3 and 6 `full`, besides the three closing
stages.

## Vendored native code

Planned, not yet vendored: [SlothDB](https://github.com/SouravRoy-ETL/slothdb)
(MIT) at commit `8180f215847015944c935f56f008d194e2c1494c`, with its own
`third_party/` subset (zstd decompressor, snappy, fastpforlib, miniz,
`unordered_dense.h`). Licences and the initial patch list are in the
design's front matter and §4.2. Fill in the vendored paths, the patch count
and the update procedure when Stage 1 vendors it.
