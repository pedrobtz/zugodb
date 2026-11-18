# SlothDB as zuslothdb's engine: an assessment

Written 2026-11-18, after Stages 0–5 and the start of Stage 6, at the
maintainer's request: what testing found about the SlothDB C++ engine,
why, and what "option A" (a narrow, hardened v0.1.0) would take. It is
the evidence for revisiting decision D1 in [design.md](design.md); the
decision itself is recorded there, not here.

The engine is SlothDB at upstream commit `8180f21` (v0.2.7+), vendored
from the fork [pedrobtz/slothdb](https://github.com/pedrobtz/slothdb),
branch `zuslothdb` ([VENDORED.md](VENDORED.md)).

## Summary

- **Correct on the happy path, often wrong off it.** Plain SQL over CSV,
  Parquet and JSON files with ordinary types, and `DATE`/`TIMESTAMP`,
  gives duckdb's answers. Almost every component tested against a
  reference beyond that had defects, and nearly all were *silent*: a
  wrong value with no error.
- **Not robust to damaged input.** A one-off spike fed 1,200 damaged
  files to a sanitizer build. The Parquet reader, the package's headline
  format, crashed the process on 191 of 600 and hung on 15. In R a crash
  ends the user's session.
- **Upstream cannot help.** It calls itself experimental, has had no
  commit since 2026-06-03, reports about 18% sqllogictest parity in its
  last commit message, and its own test suite crashes at its 95th case.
- **What it cost so far:** 16 commits on the fork (a 17th is in
  pull request #56). Most of them turn a wrong answer into an error, not
  a right answer. The pattern points one way: fast paths and
  hand-written parsers built for benchmark speed, with correctness
  checked only on the inputs the benchmarks use.

## How the evidence was gathered

| Method | Oracle | Where |
| --- | --- | --- |
| Recorded duckdb answers | duckdb 2.0.0's result for the same SQL | `tests/testthat/oracle/`, Stages 3–4 |
| Files from real writers | the values a reference writer put in: duckdb (CSV, Parquet, JSON), R's `arrow`, `openxlsx`, Python's `sqlite3` | probes, then `tests/testthat/test-parquet-types.R`, `test-json.R`, `test-arrow.R`, `test-refused.R` |
| Round trips | `identical()` to what was written | `test-roundtrip.R`, `test-storage.R` |
| Upstream's suite | its own doctest cases | `tools/upstream-tests.sh` |
| CI | ASan, UBSan, valgrind, rchk, gctorture, three OSes | `pedrobtz/r-actions` |
| Hostile-input spike | survival under ASan + UBSan; rows equal to `count(*)` | a scratch harness, below |

## Defects found

Each is either fixed by a fork commit or refused with a classed error. A
refusal stops the wrong value; it does not make the feature work.

### Storage and concurrency

| Defect | Effect | Status |
| --- | --- | --- |
| `ValidityMask` held one vector's 2048 bits; a row group (122,880 rows) kept its `NULL`s in one (#45) | heap overflow; a table with 5 `NULL`s in 5000 rows read back with 817; an `INSERT` of more than 2048 `VALUES` crashed | fixed (fork 10) |
| Stop flags set outside the mutex their waiters use (#32) | Parquet `LIMIT` queries hung at random; about 1 doctest run in 14 | fixed (fork 8) |
| `thread_local` objects with destructors in decode workers, under MinGW (#34) | every multi-threaded Parquet scan killed R on Windows | fixed (fork 9) |
| An exception in a Parquet decode worker escapes its thread | `std::terminate`: the process dies on a corrupt page (spike, 163 cases) | **open** |
| No transactions: `ROLLBACK` only clears a flag | `BEGIN … ROLLBACK` undoes nothing, silently; upstream's own test says so | refused (fork 12) |
| Database file: no checksum, read whole at open, written whole at close, no WAL | damage goes undetected; a crash loses every change since open | open; documented (design §8) |

### Types and SQL

| Defect | Status |
| --- | --- |
| `DECIMAL(p, s)` casts become `DOUBLE`; a `TIMESTAMPTZ` column becomes `TIMESTAMP` | documented; not produced |
| No list, struct or map syntax; `LIST_VALUE`, `STRUCT_PACK`, `ROW` "not implemented" | not produced |
| `HUGEINT`: the type id exists, its values are wrong (`CAST(42 AS HUGEINT)` reads as 16) | refused (design Q1) |
| `TIME`: every value the engine makes is midnight (#43) | refused |
| `STRFTIME` with one argument read past its argument list and crashed | fixed (fork 5) |
| `TINYINT` arithmetic unchecked for overflow; `substr` counts bytes; a subnormal literal and a `FLOAT` infinity refused | known differences (`oracle/known-differences.md`) |
| A `BLOB` literal keeps its `\x` escapes as text; `length()` of a blob is wrong | known difference |
| A file cannot be read inside a subquery or `CREATE TABLE … AS` ("Table '__FILE__' not found") | open |
| `typeof()`, `count(*) - count(x)` and other functions "not implemented"; no `ALTER TABLE … RENAME`, schemas or temporary tables; a quoted identifier cannot hold `"`; unquoted identifiers are case-sensitive | documented where the R API meets them |

### File readers

| Reader | Defect | Status |
| --- | --- | --- |
| Parquet | `DECIMAL` unscaled, `TIME` and ms/ns `TIMESTAMP` as raw counts, `UUID`/`INTERVAL` as `NULL`, unsigned 32/64-bit read as signed (#36–#38): the reader honoured only two annotations | refused at bind time (fork 13, 14) |
| Parquet | no nested columns at all | error ("Column not found") |
| JSON | neither of its two parsers is a JSON parser: nested values read as 0 and dropped later keys, a later key dropped, mixed types coerced, `\u` escapes broken (#39) | strict validation, `\u` decoded (fork 15) |
| Arrow IPC | a private format after Arrow's magic: reads no real Arrow file, writes none another tool reads (#40) | refused (fork 16) |
| SQLite | hand-written parser: a `REAL` 0 read as uninitialised memory, sums wrong, long text cut at overflow pages | refused (#55, pull request #56) |
| Excel | garbage after the first column, `sheet =` ignored | refused (#55, #56) |
| Avro | untested; no writer available | refused (#55, #56) |
| CSV | a doubled quote (`""`) ends the field and misreads the rest of the row; a value that does not fit the type sampled from the first 100 rows becomes garbage; quoting is parsed in five separate places, and the parallel splitter cuts at raw newlines | **open** |
| CSV | signed integer overflow in `ParseInt64` on a long digit run (undefined behaviour) | **open** (spike) |

### Upstream's own state

- Its doctest suite crashed at case 95 on pristine `main` (the
  `STRFTIME` crash), and 7 date cases were stale and failing.
- Its own round-trip tests covered the private Arrow format and a
  committed SQLite fixture, so they passed without testing
  interoperability.
- The last commit message reports sqllogictest parity of about 18%.

## The hostile-input spike

The method: the vendored engine at `zuslothdb-7`, built without R with
`-fsanitize=address,undefined`. A harness ran `SELECT *` and
`SELECT count(*)` over each file and recorded the outcome. The inputs
were 150 mutants of each seed: up to 46 truncations plus random bit flips, byte
overwrites, random chunks and duplicated runs, under a fixed random
seed. The seeds were written by duckdb, plus a database file written by
zuslothdb. The run took two minutes.

| Format | Files | Crash | Hang | Rows ≠ `count(*)` | Refused | Read |
| --- | --- | --- | --- | --- | --- | --- |
| Parquet (snappy, gzip, zstd, uncompressed) | 600 | 191 | 15 | 6 | 186 | 202 |
| CSV | 150 | 5 | 0 | 3 | 1 | 141 |
| JSON (NDJSON and array) | 300 | 0 | 0 | 0 | 281 | 19 |
| database file | 150 | 0 | 0 | n/a | 102 | 48 |

What the crashes were:

- **163 Parquet aborts:** `libc++abi: terminating due to uncaught
  exception of type slothdb::IOException: Parquet decompression
  failed`. A decode worker runs in a `std::thread` with no `try`; the
  error itself is the right one, but it ends the process.
- **28 Parquet memory and UB errors:**
  - heap-buffer-overflow while parsing metadata strings;
  - `snappy::GetUncompressedLength` called on a range whose length had
    wrapped below zero;
  - null-pointer arithmetic, SEGV and BUS from reads past the mapped
    file.

  The page-header and Thrift readers trust lengths from the file.
- **15 Parquet hangs** on uncompressed files, probably a corrupt page
  header that makes no progress.
- **5 CSV:** UBSan signed overflow in `ParseInt64`.
- **Rows ≠ `count(*)` (9):** silent partial results. For example, 2048
  rows of 3000 with no error.
- **JSON:** none. The strict validator (fork 15) refused every damaged
  file.
- **Database file:** no crash. But with no checksum, the 48 damaged
  files that opened may hold silently corrupted rows, and nothing can
  tell.

## Root causes

1. **Fast paths without shared validation.** The same format is parsed
   in several places. CSV has five quote-handling paths, JSON two
   parsers, Parquet slot, consumer and inline decode modes. Each makes
   its own shortcuts, so a fix in one place leaves the others wrong.
2. **Lengths and offsets from the file are trusted.** The Parquet
   page-header and Thrift readers, and the SQLite parser, index buffers
   by values read from the bytes.
3. **Worker threads with no error path.** Exceptions thrown on decode
   workers cannot reach the caller.
4. **Breadth before depth.** Type ids, readers and SQL functions exist
   as names, with partial or no behaviour behind them: `HUGEINT`,
   `TIME`, `DECIMAL`, nested types, Arrow, `LIST_VALUE`.
5. **Tests that cannot fail.** Round trips of the engine's own private
   formats, and doctests run only on the inputs the engine writes.

## Option A: a narrow, hardened v0.1.0

**Scope it would ship:**
- SQL over local CSV, Parquet and JSON files, and in-memory or
  file-backed databases;
- types: logical, integer, double, `VARCHAR`, `DATE`, `TIMESTAMP`,
  `BLOB` and 64-bit integers;
- `dbWriteTable()` and the rest of Stage 5's write path;
- everything else refused with a classed error, as now;
- described as experimental, and not a `duckdb` alternative (D1's
  fallback wording).

**Its bar:** no input file crashes or hangs R, and no value the package
returns is wrong without an error. The checks are ASan, UBSan and
valgrind over the suite, plus fuzzing in CI with an agreement oracle.

### Engine work (fork commits)

| # | Work | Size | Done when |
| --- | --- | --- | --- |
| A1 | Catch exceptions in every engine worker thread (Parquet slot and consumer modes, the CSV and JSON parallel parsers, `ThreadPool` tasks); re-throw on the query's thread after the join | S | the spike's 163 aborts become `zuslothdb_sql_error`; a test with a corrupt-page fixture |
| A2 | Parquet: bounds-check every length and offset read from the file (Thrift compact reader, page headers, dictionary and data pages, decompression inputs and outputs) | L | 0 crashes in a spike of 10× the size above, every codec; reviewed site by site |
| A3 | Parquet: guarantee progress on a corrupt page header (the hang) | S–M | the 15 hang inputs are errors; a timeout test |
| A4 | Parquet: a short or partial read is an error, not fewer rows | M | rows always equal `count(*)` |
| A5 | CSV: overflow-checked integer parsing (`ParseInt64`) | S | the 5 UB inputs are errors |
| A6 | CSV: a strict pre-pass on the opening thread, as for JSON, that refuses a doubled quote, a quoted newline and a value that does not fit its sampled type, naming row and column | M | the probes' wrong values become errors; recorded answers for valid files |
| A6′ | *(alternative to A6)* RFC 4180 quoting in all five parse paths, types verified over the whole file | L, risky | duckdb's answers on quoted files |
| A7 | Database file: a checksum, behind a format-version bump (Q7, from 1001), or a documented "damage undetected" | M (checksum) / S (document) | a damaged file is refused, or the README says it is not |
| A8 | Database file: harden `Checkpoint::Load` against damaged input (no crash seen yet; reachable from a hostile file) | M | fuzzed with A9 |

### Package work (the rest of Stage 6, then Stages 7–12)

| # | Work | Size |
| --- | --- | --- |
| A9 | One fuzz harness per kept reader (CSV, Parquet, JSON, database file), linked against the engine without R, with an agreement oracle; `fuzz.yaml` in CI with a reach floor | M–L |
| A10 | `zuslothdb_read()` (Q12), path tests, persistence and the golden database file (Q7), the hygiene check (no file outside `tempdir()`) | M |
| A11 | Recorded duckdb answers for each kept format and Parquet codec | S–M |
| A12 | Stages 7–12 as the roadmap has them: DBItest, documentation with measured claims, assurance, full CI, readiness, review | L |
| A13 | Close the two r-actions gaps the package's CI waits on (`r-actions#129` clang UBSan on Ubuntu 26.04, `#131` valgrind and `cli`'s thread) | S, in r-actions |

### Effort, risk and what A does not fix

- **The critical path is A2 plus A9.** A bounds audit of the
  Parquet reader (`parquet.cpp`, about 3,300 lines, its Thrift reader
  included), kept honest by fuzzing. The
  rest is small to medium. Realistically A is several more working
  sessions of engine work before Stage 6 resumes, then the six closing
  stages.
- **Maintenance does not end at release.** Upstream is inactive, so
  every future engine defect is the fork's. CRAN's checks change, new
  compilers warn, and each new fuzzing campaign may find more.
- **A leaves a small product.** No transactions, no decimals, times,
  nested types, Arrow, SQLite, Excel or Avro. Files cannot appear in
  subqueries. Much less of the engine works than upstream's README
  suggests.
- **Unknowns remain.** Only the readers were fuzzed. The SQL layer
  (planner, binder, expression executor) was tested only through the
  oracle corpus. Design §10's 10⁵-deep expressions and interrupts are
  untried.

## The alternatives, for comparison

- **D: keep the core, replace the readers.** Chosen by the maintainer on
  2026-11-18, after this assessment. The engine's file readers are
  refused and the zu family's (`zucsv`, `zujson`, `qio`) are linked in
  their place, which removes A2 to A6; the core is measured and fixed on
  the fork. The plan is in [design.md](design.md) §7 and D9, and the
  stages in [roadmap.md](roadmap.md).

- **C: stop the CRAN path.** Record D1 as "stop for CRAN". Keep
  zuslothdb on GitHub as an experimental test bed with the fork, its
  limits documented. Land A1 and A5 anyway, since they are small and stop
  the test bed from killing R sessions. Cheap, honest, and keeps
  everything built.
- **B: continue the roadmap unchanged.** Not advised: Stage 6's own
  fuzzing would find the spike's crashes and force A's engine work
  anyway.

## What option D does to the open defects

Added 2026-11-18, after D was chosen. The tables above mark some defects
**open**; this is where each goes.

| Defect | After D |
| --- | --- |
| An exception on a Parquet decode worker ends the process | The reader is refused in Stage 6 and cut in Stage 10. Catching exceptions on the workers that remain (A1) is Stage 6's |
| The Parquet spike's crashes, hangs and short reads (A2 to A4) | Gone with the reader. `qio` reads Parquet, and its own fuzz harness is a requirement of pedrobtz/qio#71 |
| CSV: doubled quotes, types sampled from 100 rows, five quote paths | The reader is refused in Stage 6. `zucsv` reads CSV: zsv handles quoting and inference is over the whole file |
| CSV: signed overflow in `ParseInt64` | The same; the code leaves with the cut |
| Database file: no checksum | A checksum, decided 2026-11-18 (design §8), in Stage 6. There is still no write-ahead log, and the README says so |
| A file cannot be read inside a subquery or `CREATE TABLE … AS` | Nothing reads a file from SQL until design §16.5's step B, which has to make it work |

## Review notes

Added 2026-11-18, from a second read of this document against the
repository.

- **"Most of them turn a wrong answer into an error"** (Summary) is
  loose. Of the 16 fork commits, 5 are refusals, 4 are fixes, 6 are
  packaging or policy and 1 updates stale tests. It holds for the
  commits a defect drove: 5 of 9, or 6 of 10 with pull request #56.
- **`test-refused.R` and the spike's `zuslothdb-7` build** exist only in
  pull request #56, not on `main`.
- **A doubled quote is ordinary RFC 4180**, so the CSV defect is on the
  happy path, and the Summary's first bullet undersells it.
- **A1's size, "S", may be low.** `physical_planner.cpp` starts raw
  `std::thread`s at a dozen or more sites beyond the ones A1 names. Which
  of them can throw was not checked.
- **"What A does not fix" leaves out** that the engine has no prepared
  statements (Q3), no query cancellation (Q6) and no memory limit (Q4).
- **The rows marked open had no issue of their own** when this was
  written. The table above says where each now goes.
