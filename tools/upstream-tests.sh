#!/bin/sh
# tools/upstream-tests.sh [workdir]: upstream SlothDB's own doctest suite,
# built against the package's vendored, patched tree (src/vendor/slothdb)
# with R's compilers and flags, without R or CMake. Run from the package
# root. Needs git and network access the first time (the test sources and
# doctest are not vendored: they come from upstream at the pinned commit,
# with tools/vendor.yaml's test_patches applied and test_exclude left out).
#
# Exit status: the doctest binary's, so a failing or crashing upstream case
# fails the step. design §4.2 / D1: the patches must break nothing upstream
# tested.
set -eu
here=$(pwd)
work=${1:-$(mktemp -d)}
jobs=${SDB_JOBS:-4}
R=${R_HOME:+$R_HOME/bin/}R
command -v R >/dev/null 2>&1 && R=R

repo=$(awk '/^  repository:/ {print $2}' tools/vendor.yaml)
commit=$(awk '/^  commit:/ {print $2}' tools/vendor.yaml)
repo=${SDB_UPSTREAM:-$repo}
[ -n "$repo" ] && [ -n "$commit" ] || { echo "cannot read tools/vendor.yaml" >&2; exit 2; }

# Upstream's test tree at the pinned commit, test patches applied.
up="$work/upstream"
if [ ! -d "$up/.git" ]; then
  git -c core.autocrlf=false clone -q --no-checkout "$repo" "$up"
  git -C "$up" config core.autocrlf false
fi
git -C "$up" -c advice.detachedHead=false checkout -q -f "$commit"
git -C "$up" clean -q -fdx test
awk '/^test_patches:/ {on = 1; next} /^[a-z_]+:/ {on = 0}
     on && /^  - file:/ {print $3}' tools/vendor.yaml | while read -r p; do
  git -C "$up" apply --whitespace=nowarn "$here/tools/patches/$p"
  echo "applied test patch $p"
done
excluded=$(awk '/^test_exclude:/ {on = 1; next} /^[a-z_]+:/ {on = 0}
     on && /^  - path:/ {print $3}' tools/vendor.yaml)

# The vendored translation units, exactly as src/Makevars lists them.
units=$(sed -n '/^OBJECTS = /,/[^\\]$/p' src/Makevars | tr -d '\\' |
  sed 's/^OBJECTS = //' | tr ' ' '\n' | grep '^vendor/' | sed 's/\.o$//')

CC=$("$R" CMD config CC)
CXX=$("$R" CMD config CXX20)
CFLAGS=$("$R" CMD config CFLAGS)
CXXFLAGS="$("$R" CMD config CXX20STD) $("$R" CMD config CXX20FLAGS)"
v="$here/src/vendor/slothdb"
defs="-DNDEBUG -DZSTD_DISABLE_ASM=1 -DZSTD_MULTITHREAD=0"
incs="-I$v/include -I$v/third_party/zstd/lib -I$v/third_party/fastpforlib -I$v/third_party/snappy"
echo "CC = $CC $CFLAGS"
echo "CXX = $CXX $CXXFLAGS"

obj="$work/obj"
mkdir -p "$obj"
# One line per compile: "<compiler and flags> <source> <object>".
list="$work/compile.txt"
: > "$list"
for u in $units; do
  for ext in cpp cc c; do
    [ -f "src/$u.$ext" ] || continue
    o="$obj/$(echo "$u" | tr / _).o"
    case $ext in
      c) echo "$CC $CFLAGS $defs $incs -c $here/src/$u.$ext -o $o" >> "$list" ;;
      *) echo "$CXX $CXXFLAGS $defs $incs -c $here/src/$u.$ext -o $o" >> "$list" ;;
    esac
  done
done
for t in $(cd "$up" && find test/unit -name '*.cpp' | LC_ALL=C sort); do
  skip=""
  for e in $excluded; do [ "$t" = "$e" ] && skip=1; done
  if [ -n "$skip" ]; then echo "excluded $t"; continue; fi
  o="$obj/$(echo "$t" | tr / _).o"
  echo "$CXX $CXXFLAGS $defs $incs -I$up/third_party/doctest -c $up/$t -o $o" >> "$list"
done
echo "compiling $(wc -l < "$list" | tr -d ' ') translation units, $jobs at a time"
# Each line is a full command; a failure stops xargs with a non-zero status.
tr '\n' '\0' < "$list" | xargs -0 -P "$jobs" -n 1 sh -c 'eval "$0" || exit 255'

exe="$work/slothdb_tests"
case "$(uname -s)" in MINGW* | MSYS* | CYGWIN*) exe="$exe.exe" ;; esac
$CXX $CXXFLAGS "$obj"/*.o -o "$exe" -pthread
cd "$up"
"$exe" --no-intro=true --duration=false
