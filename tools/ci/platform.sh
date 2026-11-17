#!/bin/sh
# tools/ci/platform.sh <os>: zuslothdb's per-OS checks, run once on each
# runner of platforms.yaml (os is macos, windows or ubuntu), with
# R_ACTIONS_PROFILE set to quick or full. Every check is a step here, in
# order; set -e names the one that failed. Run from the package root; it
# also runs locally.
#
#   1. The package installs (the vendored engine with R's compiler).
#   2. Its shared object exports R_init_zuslothdb and nothing of SlothDB's
#      or its third-party code, and links no symbol CRAN's compiled-code
#      check flags.
#   3. zuslothdb and duckdb in one session, in both load orders.
#   4. Upstream's own doctest suite on the vendored, patched tree
#      (tools/upstream-tests.sh): on Linux always, on macOS and Windows
#      under the full profile; ten runs of it under the full profile.
set -eu
os=${1:?usage: tools/ci/platform.sh <macos|windows|ubuntu>}
case "$os" in
  macos | windows | ubuntu) ;;
  *) echo "unknown os: $os" >&2; exit 2 ;;
esac
profile=${R_ACTIONS_PROFILE:-quick}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
R=${R_HOME:+$R_HOME/bin/}R
Rscript=${R_HOME:+$R_HOME/bin/}Rscript
command -v Rscript >/dev/null 2>&1 && R=R && Rscript=Rscript
jobs=$("$Rscript" -e 'cat(max(1L, parallel::detectCores()))')
sep=:
[ "$os" = windows ] && sep=';'

group() { echo "::group::$*"; }
endgroup() { echo "::endgroup::"; }

group "Toolchain"
"$Rscript" -e 'cat(R.version.string, R.version$arch, "\n")'
echo "CXX20 = $("$R" CMD config CXX20) $("$R" CMD config CXX20STD)"
echo "CXX20FLAGS = $("$R" CMD config CXX20FLAGS)"
$("$R" CMD config CXX20) --version | head -1
uname -m
echo "cores: $jobs, profile: $profile"
endgroup

# ---------------------------------------------------------------------------
# 1. Install, timed: the cold source build a user pays (design D1), here
# with $jobs make jobs.
group "Install"
lib="$tmp/lib"
mkdir -p "$lib"
start=$(date +%s)
MAKEFLAGS="-j$jobs" "$R" CMD INSTALL --preclean --no-test-load --library="$lib" . \
  > "$tmp/install.log" 2>&1 || { cat "$tmp/install.log"; exit 1; }
echo "installed in $(( $(date +%s) - start )) s with -j$jobs"
grep -c 'warning:' "$tmp/install.log" | sed 's/^/compiler warnings: /' || true
endgroup
so=$(ls "$lib"/zuslothdb/libs/zuslothdb.so "$lib"/zuslothdb/libs/*/zuslothdb.dll \
  "$lib"/zuslothdb/libs/zuslothdb.dll 2>/dev/null | head -1)
[ -n "$so" ] || { echo "no shared object installed" >&2; exit 1; }
echo "shared object: $so ($(wc -c < "$so" | tr -d ' ') bytes)"

# ---------------------------------------------------------------------------
# 2. Symbols. Defined, exported symbols and undefined (imported) ones, one
# per line.
exports() {
  case "$os" in
    macos) nm -gU "$1" | awk '{print $NF}' | sed 's/^_//' ;;
    windows) objdump -p "$1" | awk '/^\[Ordinal\/Name Pointer\] Table/ {on = 1; next}
                                    on && /^\t\[/ {print $NF; next} on && NF == 0 {on = 0}' ;;
    *) nm -D --defined-only "$1" | awk '{print $NF}' ;;
  esac
}
imports() {
  case "$os" in
    macos) nm -u "$1" | awk '{print $NF}' | sed 's/^_//' ;;
    # "<tab><address>  <ordinal>  <hint>  <name>" under each DLL Name.
    windows) objdump -p "$1" | awk '/DLL Name:/ {on = 1; next}
                                    on && /^\t[0-9a-f]+[ \t]/ {print $NF}' ;;
    *) nm -u "$1" | awk '{print $NF}' | sed 's/@.*//' ;;
  esac
}

group "Exported symbols"
exports "$so" | sort -u > "$tmp/exports"
wc -l < "$tmp/exports" | sed 's/^ */exported: /'
endgroup
# The C++ standard library's template instantiations (std:: and
# __gnu_cxx::, with their typeinfo, vtables and guard variables) are
# exported by libstdc++ and libc++ whatever -fvisibility says. Each is the
# same code in every library that instantiates it, so binding to another
# library's copy changes nothing. They are told apart by the mangled name,
# whose outermost scope follows _Z (or _ZN and its cv-qualifiers): St for
# std, 9__gnu_cxx for __gnu_cxx. A demangled name will not do: it starts
# with the return type. Everything else must be R_init_zuslothdb (design
# §4, the duckdb_fastpforlib and zstd clashes).
std='^_Z(T[ISVT]|GV|TH|TW)?Z?N?[rVKRO]*(St|9__gnu_cxx)'
grep -vE "$std" "$tmp/exports" > "$tmp/ours" || true
echo "of which not the C++ standard library's: $(wc -l < "$tmp/ours" | tr -d ' ')"
if ! [ "$(cat "$tmp/ours")" = "R_init_zuslothdb" ]; then
  echo "FAIL: the shared object exports more than R_init_zuslothdb:" >&2
  grep -vx 'R_init_zuslothdb' "$tmp/ours" | head -200 >&2
  exit 1
fi
echo "ok: R_init_zuslothdb is the only symbol of the package's code exported"

group "Imported symbols"
imports "$so" | sort -u > "$tmp/imports"
wc -l < "$tmp/imports" | sed 's/^ */imported: /'
endgroup
# The library symbols CRAN's "checking compiled code" flags, by base name:
# a leading underscore, a _chk suffix and the platform's spelling of stderr
# and stdout stripped (tools/patches/0002-no-process-output.patch).
sed -e 's/^_*//' -e 's/_chk$//' \
  -e 's/^stderrp$/stderr/' -e 's/^stdoutp$/stdout/' \
  -e 's/^acrt_iob_func$/stderr/' -e 's/^stdio_common_vsprintf$/sprintf/' \
  -e 's/^assert_fail$/abort/' -e 's/^assert_rtn$/abort/' -e 's/^assert$/abort/' \
  "$tmp/imports" | sort -u > "$tmp/bases"
flagged=$(grep -xE 'exit|_exit|abort|printf|vprintf|puts|putchar|sprintf|vsprintf|rand|srand|stderr|stdout|system' \
  "$tmp/bases" || true)
# On Windows, Rtools links libstdc++ statically, so the C++ runtime's own
# abort, exit, sprintf and stderr are in every C++ package's DLL, and R's
# check does not report them there. The package's code is held to the
# rule on macOS and Linux, where the C++ runtime is a shared library; here
# the list is printed, not gated.
if [ -n "$flagged" ] && [ "$os" = windows ]; then
  echo "note: linked through the static C++ runtime: $(echo $flagged)"
elif [ -n "$flagged" ]; then
  echo "FAIL: links CRAN-flagged symbols: $(echo $flagged)" >&2
  exit 1
else
  echo "ok: no CRAN-flagged symbol linked"
fi

# ---------------------------------------------------------------------------
# 3. With duckdb in the same session. A binary from the platform's usual
# repository: CRAN's on macOS and Windows, Posit's on Ubuntu.
group "duckdb"
R_LIBS="$lib${R_LIBS:+$sep$R_LIBS}" "$Rscript" tools/ci/install-duckdb.R
endgroup
for order in zuslothdb-first duckdb-first; do
  R_LIBS="$lib${R_LIBS:+$sep$R_LIBS}" "$Rscript" tools/ci/coexist.R "$order"
done

# ---------------------------------------------------------------------------
# 4. Upstream's own tests on the patched tree. Linux on every run; macOS and
# Windows (where the MinGW patch is what is tested) under the full profile.
if [ "$os" = ubuntu ] || [ "$profile" = full ]; then
  group "Upstream doctest suite"
  # Ten runs under the full profile, so a race like #32 shows.
  runs=1
  [ "$profile" = full ] && runs=10
  SDB_JOBS="$jobs" SDB_TEST_RUNS="$runs" sh tools/upstream-tests.sh "$tmp/upstream-tests"
  endgroup
fi

echo "platform.sh: $os done"
