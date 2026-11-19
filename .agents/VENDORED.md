# Vendored code

zuslothdb bundles its engine, libzugodb, with the third-party code it
carries, under `src/vendor/libzugodb/`. Do not edit it as ordinary package
code: change libzugodb, release it, then re-vendor.

| Library | Version | Commit | Retrieved | License |
|---|---|---|---|---|
| [libzugodb](https://github.com/pedrobtz/libzugodb) | `v0.1.1` | `d3ec8c5ff1818c5079ecac0185514656fb715e66` | 2026-11-18 | MIT (`src/vendor/libzugodb/LICENSE`) |
| derived from [SlothDB](https://github.com/SouravRoy-ETL/slothdb) | v0.2.7+ | `8180f215847015944c935f56f008d194e2c1494c` | 2026-11-17 | MIT, "SlothDB Contributors" |

The third-party code inside it (zstd's decompressor, snappy, fastpforlib,
miniz, `unordered_dense.h`) came with SlothDB; its holders are in
`inst/COPYRIGHTS`. `tools/vendor.yaml` is the machine-readable form of this
table, and what `tools/vendor.R` reads.

## libzugodb

Since 2026-11-18 (design D12) the engine is a project of its own:
`pedrobtz/libzugodb`, which was the fork `pedrobtz/slothdb`, renamed with
its history kept.

- **`main`** is the engine: SlothDB at `8180f21` plus the changes in its
  `CHANGELOG.md`. Up to `zuslothdb-7` they were one commit each, with their
  reasons in the commit messages and in this file's history; since
  `v0.1.0` it is an ordinary project, changed by pull requests.
- **`upstream-mirror`** is SlothDB's `main` at `8180f21`, kept for cutting
  branches to offer upstream.
- **`fix/…`** and **`feat/…`** are those branches (below). The old
  `zuslothdb` branch and its `zuslothdb-<n>` tags stay for the history.
- **Releases** are tags `v<x.y.z>`. Its CI runs the doctest suite on Linux
  (gcc, and ASan with UBSan), macOS (clang) and Windows (MinGW-w64 UCRT64,
  as Rtools).

What libzugodb removed from SlothDB, beyond the refusals its changelog
lists: the HTTP client, the extension loader and the `ask` module (D4,
D11), and the C++ namespace `slothdb::` became `zugodb::`.

## Included source

From libzugodb at the tag, by `tools/vendor.R`: `LICENSE`, `include/`,
`src/` and the `third_party/` subset `tools/vendor.yaml` lists under
`copy`, minus its `exclude` entries, each with its reason: `src/api/` (the
C API; the glue binds the C++ one, D2) and the CMake files.
`tools/vendor.R` writes `src/Makevars`, `src/Makevars.win`,
`src/sdb_provenance.h`, `inst/vendor.yaml` and
`tools/vendor/checksums.md5` from the result; never edit them.

## Drift

```sh
tools/vendor/verify     # Rscript tools/vendor.R --verify
```

It clones libzugodb and asserts that the tag is the pinned commit, that
SlothDB's `8180f21` is an ancestor of it, and that `src/vendor/libzugodb`
and the generated files match it, file for file. The `vendor` workflow
runs it on pull requests and weekly. `tools/upstream-tests.sh` builds
libzugodb's doctest suite, at the pin, against the vendored tree with R's
compilers and flags.

## Changing the engine

1. A pull request to libzugodb, with its test; its CI green.
2. A release tag there.
3. `source.tag` and `source.commit` in `tools/vendor.yaml` and the table
   above; `Rscript tools/vendor.R`; `tools/upstream-tests.sh`; the full CI
   profile here.

The `vendor-upstream` workflow still watches SlothDB for a release past
`v0.2.7`; a change there worth having is cherry-picked into libzugodb.

## Upstream reporting

**Reported on 2026-11-18** (maintainer's say): fourteen issues, each
reproduced first on pristine upstream `8180f21` with upstream's own CMake
build and shell, listed below. The six fix branches were then opened as
upstream pull requests (SouravRoy-ETL/slothdb#43 to #48), on the
maintainer's say.

A fix worth offering upstream is cut as a branch of libzugodb from
`upstream-mirror` (SlothDB's `main` at `8180f21`), so that it carries one
change and nothing else, with zuslothdb-specific comments rewritten and
the namespace kept as `slothdb::`. Write the regression test against
pristine upstream first: it tells whether the bug is upstream's.

### Prepared branches

Each is one commit on upstream `main` at `8180f21` (libzugodb's
`upstream-mirror` branch), with the zuslothdb
comments scrubbed, built with upstream's own CMake (Release, macOS x86-64,
2026-11-18). **No pull request has been opened.**

Upstream's own suite (`slothdb_tests`, 408 cases) cannot run whole on
pristine `main`: "Advanced - STRFTIME" crashes it (SIGSEGV), and doctest
stops after 95 cases. So each branch was run with that case excluded and
its failing cases compared with `main`'s, which are the seven stale
`DATE` cases (`LAST_DAY` ×3, `MAKE_DATE` ×3, `Timestamp - CURRENT_DATE`).
Every branch fails exactly those seven and nothing else, except
`fix/stale-date-tests`, which passes all 407; `fix/strftime-one-argument`
also passes the STRFTIME case, which crashes on `main`.

| Branch | Commit | Upstream issue | Notes |
|---|---|---|---|
| `fix/validity-capacity` | `0028b79` | [#29](https://github.com/SouravRoy-ETL/slothdb/issues/29), PR [#43](https://github.com/SouravRoy-ETL/slothdb/pull/43) | Heap overrun and wrong results past row 2048. Send this one first. Upstream has no test that reaches it; one should come with the pull request. |
| `fix/lost-wakeup` | `a824493` | [#31](https://github.com/SouravRoy-ETL/slothdb/issues/31), PR [#46](https://github.com/SouravRoy-ETL/slothdb/pull/46) | Intermittent hang, about 1 doctest run in 14 on Linux. No deterministic upstream test. |
| `fix/strftime-one-argument` | `5905fc9` | [#30](https://github.com/SouravRoy-ETL/slothdb/issues/30), PR [#44](https://github.com/SouravRoy-ETL/slothdb/pull/44) | Crash in upstream's own "Advanced - STRFTIME" case; carries that case's change. |
| `fix/mingw-thread-local` | `b321a8d` | [#32](https://github.com/SouravRoy-ETL/slothdb/issues/32), PR [#48](https://github.com/SouravRoy-ETL/slothdb/pull/48) | Windows-only crash under MinGW-w64; needs a Windows build to show. |
| `fix/mingw-time-functions` | `c4cc471` | [#32](https://github.com/SouravRoy-ETL/slothdb/issues/32), PR [#47](https://github.com/SouravRoy-ETL/slothdb/pull/47) | MinGW-w64 build. |
| `fix/stale-date-tests` | `77cecee` | [#30](https://github.com/SouravRoy-ETL/slothdb/issues/30), PR [#45](https://github.com/SouravRoy-ETL/slothdb/pull/45) | The seven cases that fail on `main`. |
| `fix/compiler-warnings` | `ba661ba` | not reported | Two warnings; no behaviour change. |
| `feat/read-only-database` | `88d3044` | not reported | API addition, not a fix; offer last. |

Record the upstream issue or pull request number beside any branch that is
reported, so a re-vendor can tell what upstream already knows about.

### Reported without a branch

The defects zuslothdb refuses rather than fixes, reported so upstream
knows of them. Each issue names its reproduction.

| Upstream issue | Defect | Here |
| --- | --- | --- |
| [#33](https://github.com/SouravRoy-ETL/slothdb/issues/33) | An exception on a Parquet decode worker terminates the process | assessment A1 |
| [#34](https://github.com/SouravRoy-ETL/slothdb/issues/34) | Parquet logical types ignored | fork changes 13, 14 (#36 to #38) |
| [#35](https://github.com/SouravRoy-ETL/slothdb/issues/35) | `read_json` misreads nested values, late keys, mixed types, `\u` | fork change 15 (#39) |
| [#36](https://github.com/SouravRoy-ETL/slothdb/issues/36) | `read_csv`: doubled quotes, sampled types, `ParseInt64` overflow | assessment, open |
| [#37](https://github.com/SouravRoy-ETL/slothdb/issues/37) | Arrow IPC is a private format | fork change 16 (#40) |
| [#38](https://github.com/SouravRoy-ETL/slothdb/issues/38) | `sqlite_scan` wrong values | pull request #56 (#55) |
| [#39](https://github.com/SouravRoy-ETL/slothdb/issues/39) | `ROLLBACK` undoes nothing | fork change 12 (#7) |
| [#40](https://github.com/SouravRoy-ETL/slothdb/issues/40) | `TIME` all midnight, `HUGEINT` wrong | #43, design Q1 |
| [#41](https://github.com/SouravRoy-ETL/slothdb/issues/41) | `substr`, `TINYINT` overflow, subnormal literal, `FLOAT` infinity, `BLOB` escapes | `tests/testthat/oracle/known-differences.md` |
| [#42](https://github.com/SouravRoy-ETL/slothdb/issues/42) | A table function in a subquery or `CREATE TABLE AS` | design D1 |

The Excel reader was not reported: on a file from `openxlsx` it refuses
compressed worksheets with a clear error, which is not a wrong value.
