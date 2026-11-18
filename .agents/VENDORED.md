# Vendored code

zuslothdb bundles the SlothDB engine, with the third-party code it carries,
under `src/vendor/slothdb/`. Do not edit it as ordinary package code: change
it on the SlothDB fork, then re-vendor from it.

| Library | Version | Commit | Retrieved | License |
|---|---|---|---|---|
| [SlothDB](https://github.com/SouravRoy-ETL/slothdb) | v0.2.7+ | `8180f215847015944c935f56f008d194e2c1494c` | 2026-11-17 | MIT (`src/vendor/slothdb/LICENSE`) |
| [SlothDB fork](https://github.com/pedrobtz/slothdb) | `zuslothdb` branch, tag `zuslothdb-6` | `1b126916a47074be6339f64c81d5b3929b2e495e` | 2026-11-18 | MIT (`src/vendor/slothdb/LICENSE`) |

The third-party code inside it (zstd's decompressor, snappy, fastpforlib,
miniz, `unordered_dense.h`) comes with SlothDB at that commit; its holders
are in `inst/COPYRIGHTS`. `tools/vendor.yaml` is the machine-readable form of
this table and of the patch ledger, and what `tools/vendor.R` reads.

## The SlothDB fork

zuslothdb does not vendor from upstream SlothDB directly. It vendors from
[`pedrobtz/slothdb`](https://github.com/pedrobtz/slothdb), a fork that sits
between this package and upstream (design D3, decided 2026-11-18: upstream
has had no commit since the pin, and the changes outgrew a patch stack):

```
main        ──●──●──●──●   mirror of SouravRoy-ETL/slothdb, kept current
                 │
 zuslothdb       └──●──●──●──●   the upstream pin plus one commit per local patch
                    │
 fix/…              └──●          one branch, one commit, one upstream pull request
```

- **`main`** is a pure mirror. It is only ever fast-forwarded from upstream,
  so GitHub's fork-sync works and every topic branch starts from pristine
  code.
- **`zuslothdb`** is what `src/vendor/slothdb` is copied from. It is based on
  the *upstream pin* above, not on `main`'s tip, and carries exactly one
  commit per entry in the patch ledger (`tools/vendor.yaml`'s `changes`).
  `git log <upstream-pin>..zuslothdb` is the ledger in executable form. Each
  vendoring is tagged `zuslothdb-<n>`.
- **`fix/…`** and **`feat/…`** branches are cut from `main` for upstreaming.
  See [Upstream reporting](#upstream-reporting).

Every local change is then a reviewable commit whose message explains it,
so offering one upstream is a cherry-pick, and re-vendoring onto a new
upstream is a `git rebase` that reports a conflict where upstream changed
the same code and drops a patch upstream has since fixed.

While upstream stays quiet, the fork is where the engine is maintained:
new fixes land as commits on `zuslothdb`, whether or not they are ever
offered upstream.

## Included source

From the fork at the pin, by `tools/vendor.R`: `LICENSE`, `include/`, `src/`
and the `third_party/` subset `tools/vendor.yaml` lists under `copy`, minus
its `exclude` entries, each with its reason: `src/ask/` (downloads a model),
`src/extension/` and the HTTP client (offline only, D4), `src/api/` (the C
API; the glue binds the C++ one, D2) and the CMake files. `tools/vendor.R`
writes `src/Makevars`, `src/Makevars.win`, `src/sdb_provenance.h`,
`inst/vendor.yaml` and `tools/vendor/checksums.md5` from the result; never
edit them.

## Local SlothDB patches

The ledger is `tools/vendor.yaml`'s `changes`: each commit on the fork's
`zuslothdb` branch, in order, with its subject and its reason. The commit is
the content; the entry is the rationale. The design's §4.2 has the longer
account of each, numbered as they were when they were patch files: patch *n*
is the *n*-th commit after the upstream pin.

| # | Commit | Change | Kind | Upstream branch |
|---|---|---|---|---|
| 1 | `2ac3d1c` | Take the Windows branch under MinGW, not only MSVC | portability | `fix/mingw-time-functions` |
| 2 | `c99149e` | Write nothing to stderr, never abort, use no rand() | CRAN's compiled-code check | none: R-specific |
| 3 | `b850a18` | Refuse remote paths: offline only | package policy (D4) | none: removes a feature |
| 4 | `c3255c6` | Cap every thread pool with one process-wide limit | package policy (D5) | none: replaces upstream's `SLOTHDB_MAX_THREADS` |
| 5 | `efcaea8` | Refuse STRFTIME with one argument | crash (#3) | `fix/strftime-one-argument` |
| 6 | `87a5a57` | Fix two warnings R CMD check counts | compiler warnings | `fix/compiler-warnings` |
| 7 | `b963b8e` | Add a read-only database mode | API addition | `feat/read-only-database` |
| 8 | `1f52a05` | Set stop flags under the mutex their waiters use | hang (#32) | `fix/lost-wakeup` |
| 9 | `f976710` | Keep the Parquet decoder's scratch out of thread_local | crash on Windows (#34) | `fix/mingw-thread-local` |
| 10 | `d8bf307` | Size validity masks by their holder's capacity | memory corruption, wrong results (#45) | `fix/validity-capacity` |
| 11 | `db025dd` | Update seven doctests to DATE as days since 1970 | stale tests | `fix/stale-date-tests` (its date cases), and the STRFTIME case with `fix/strftime-one-argument` |
| 12 | `6af32c5` | Make ROLLBACK say that nothing was undone | no undo: a silent wrong result (#7) | none yet: a behaviour choice upstream may make differently (upstream's own test documents the missing undo) |
| 13 | `89f52bb` | Refuse Parquet columns the reader cannot decode faithfully | silent wrong values (#36, #37, #38) | none yet: a refusal; upstream would want the decoding itself |
| 14 | `1df811d` | Refuse unreadable Parquet columns when bound, not when scanned | follow-up to 13: the refusal fires exactly when a query names the column | with 13 |
| 15 | `d5c7e62` | Refuse JSON the readers would misread, and decode \u escapes | silent wrong values (#39) | the `\u` decoding could be offered on its own; the refusal is a stopgap |
| 16 | `1b12691` | Refuse Arrow IPC: SlothDB's Arrow format is its own | no interoperability, garbage on plausible input (#40) | none: upstream would want a real reader |

## Drift

```sh
tools/vendor/verify     # Rscript tools/vendor.R --verify
```

It clones the fork and asserts:

1. `src/vendor/slothdb` and the generated files match the fork at the fork
   pin, file for file.
2. The upstream pin is an ancestor of the fork pin, so the series really is
   built on the recorded upstream commit.
3. The fork's commits between the two are exactly the ledger's, in order,
   with the same subjects, so a commit added to the fork without a ledger
   entry, or an entry without a commit, fails here.

The `vendor` workflow runs it on pull requests and weekly. A vendored file
edited in place, rather than on the fork, fails CI. `tools/upstream-tests.sh`
builds the fork's own doctest suite, at the fork pin, against the vendored
tree.

**Never edit `src/vendor/slothdb` directly.** Make the change on the fork's
`zuslothdb` branch (one commit, its reason in the message, the zuslothdb
issue as `pedrobtz/zuslothdb#n`), tag it `zuslothdb-<n+1>`, push both, add
the commit to `changes` in `tools/vendor.yaml`, set `source.tag` and
`source.commit`, update the fork row above, and run `Rscript tools/vendor.R`
and `tools/upstream-tests.sh`. If the change is upstream's defect too, cut its
`fix/…` branch as below.

## Re-vendoring a new upstream

1. Fast-forward the fork's `main` from upstream.
2. Rebase `zuslothdb` onto the new upstream commit; drop any commit upstream
   now has, and its ledger entry.
3. Update `upstream.commit` and `describe`, and the source pin and tag, in
   `tools/vendor.yaml` and the table above.
4. `Rscript tools/vendor.R`, `tools/upstream-tests.sh`, the full CI profile.

## Upstream reporting

**Nothing has been reported or offered upstream.** That needs the
maintainer's say each time. The branches below are prepared so that it is a
push and a pull request, not a reconstruction.

A branch is cut from `main`, not from `zuslothdb`, so that it carries one
change and nothing else:

```sh
git fetch upstream && git checkout main && git merge --ff-only upstream/main
git checkout -b fix/short-name main
git cherry-pick <commit-from-the-zuslothdb-branch>
# scrub the zuslothdb-specific comments (below), then:
git push -u origin fix/short-name
gh pr create --repo SouravRoy-ETL/slothdb --base main
```

Before pushing, fix two things in the cherry-picked commit:

- The `zuslothdb` branch's comments say `zuslothdb:` and cite this
  repository's issues (`#45`). Those are meaningful here and meaningless
  upstream: rewrite them to describe the code rather than its provenance.
  The `zuslothdb` branch keeps them verbatim, because the drift check
  requires it to match `src/vendor/slothdb` exactly.
- A cherry-pick onto a later upstream is not automatically still correct;
  rebuild and rerun upstream's suite on the branch.

Write the regression test against pristine upstream before reporting: the
test is what tells you whether the bug is upstream's or zuslothdb's.

### Prepared branches

Each is one commit on upstream `main` at `8180f21`, with the zuslothdb
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

| Branch | Commit | Notes |
|---|---|---|
| `fix/validity-capacity` | `0028b79` | Heap overrun and wrong results past row 2048. Send this one first. Upstream has no test that reaches it; one should come with the pull request. |
| `fix/lost-wakeup` | `a824493` | Intermittent hang, about 1 doctest run in 14 on Linux. No deterministic upstream test. |
| `fix/strftime-one-argument` | `5905fc9` | Crash in upstream's own "Advanced - STRFTIME" case; carries that case's change. |
| `fix/mingw-thread-local` | `b321a8d` | Windows-only crash under MinGW-w64; needs a Windows build to show. |
| `fix/mingw-time-functions` | `c4cc471` | MinGW-w64 build. |
| `fix/stale-date-tests` | `77cecee` | The seven cases that fail on `main`. |
| `fix/compiler-warnings` | `ba661ba` | Two warnings; no behaviour change. |
| `feat/read-only-database` | `88d3044` | API addition, not a fix; offer last. |

Record the upstream issue or pull request number beside any branch that is
reported, so a re-vendor can tell what upstream already knows about.
