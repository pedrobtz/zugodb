#!/bin/sh
# tools/upstream-tests.sh [workdir]: libzugodb's doctest suite, built
# against the package's vendored tree (src/vendor/libzugodb) with R's
# compilers and flags, without R or CMake. Run from the package root. Needs
# git and network access the first time (the test sources and doctest are
# not vendored: they come from libzugodb at the commit tools/vendor.yaml's
# `source` names, with any `test_exclude` left out).
#
# Exit status: the doctest binary's (124 for a run that hangs), so a
# failing, crashing or hanging case fails the step: the package's build
# flags must break nothing the engine's own suite tests.
set -eu
here=$(pwd)
work=${1:-$(mktemp -d)}
jobs=${ZUG_JOBS:-4}
R=${R_HOME:+$R_HOME/bin/}R
command -v R >/dev/null 2>&1 && R=R

# The `source:` block: libzugodb and its commit.
field() {
  awk -v key="$1" '/^source:/ {on = 1; next} /^[a-z_]+:/ {on = 0}
                   on && $1 == key ":" {print $2; exit}' tools/vendor.yaml
}
repo=$(field repository)
commit=$(field commit)
repo=${ZUG_SOURCE:-$repo}
[ -n "$repo" ] && [ -n "$commit" ] || { echo "cannot read tools/vendor.yaml" >&2; exit 2; }

# The fork's test tree at the vendored commit (a clone left in `workdir`
# by an earlier run is fetched into when it lacks the commit).
up="$work/upstream"
if [ ! -d "$up/.git" ]; then
  git -c core.autocrlf=false clone -q --no-checkout "$repo" "$up"
  git -C "$up" config core.autocrlf false
fi
git -C "$up" cat-file -e "$commit^{commit}" 2>/dev/null ||
  git -C "$up" fetch -q "$repo" '+refs/heads/*:refs/remotes/source/*' '+refs/tags/*:refs/tags/*'
git -C "$up" -c advice.detachedHead=false checkout -q -f "$commit"
git -C "$up" clean -q -fdx test
excluded=$(awk '/^test_exclude:/ {on = 1; next} /^[a-z_]+:/ {on = 0}
     on && /^  - path:/ {print $3}' tools/vendor.yaml)

# The vendored translation units, exactly as src/Makevars lists them.
units=$(sed -n '/^OBJECTS = /,/[^\\]$/p' src/Makevars | tr -d '\\' |
  sed 's/^OBJECTS = //' | tr ' ' '\n' | grep '^vendor/' | sed 's/\.o$//')

CC=$("$R" CMD config CC)
CXX=$("$R" CMD config CXX20)
CFLAGS=$("$R" CMD config CFLAGS)
CXXFLAGS="$("$R" CMD config CXX20STD) $("$R" CMD config CXX20FLAGS)"
v="$here/src/vendor/libzugodb"
defs="-DNDEBUG"
incs="-I$v/include"
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
# As libzugodb's test/CMakeLists.txt builds the suite without the
# conformance runner (ZUGODB_BUILD_SLT off): test/unit/tools/ left out, and
# ZUGODB_SOURCE_DIR the checkout, where the tests find their fixtures and
# read src/ and include/ (the same files as the vendored tree, which
# tools/vendor/verify checks).
for t in $(cd "$up" && find test/unit -name '*.cpp' | grep -v '^test/unit/tools/' | LC_ALL=C sort); do
  skip=""
  for e in $excluded; do [ "$t" = "$e" ] && skip=1; done
  if [ -n "$skip" ]; then echo "excluded $t"; continue; fi
  o="$obj/$(echo "$t" | tr / _).o"
  echo "$CXX $CXXFLAGS $defs -DZUGODB_SOURCE_DIR='\"$up\"' $incs -I$up/third_party/doctest -c $up/$t -o $o" >> "$list"
done
echo "compiling $(wc -l < "$list" | tr -d ' ') translation units, $jobs at a time"
# Each line is a full command; a failure stops xargs with a non-zero status.
tr '\n' '\0' < "$list" | xargs -0 -P "$jobs" -n 1 sh -c 'eval "$0" || exit 255'

exe="$work/zugodb_tests"
case "$(uname -s)" in MINGW* | MSYS* | CYGWIN*) exe="$exe.exe" ;; esac
$CXX $CXXFLAGS "$obj"/*.o -o "$exe" -pthread
cd "$up"
# ZUG_TEST_RUNS runs (1 by default), each killed after ZUG_TEST_TIMEOUT
# seconds (600): a hang fails the step with a message, not the job's
# timeout (#32, a lost wakeup that hung 1 run in about 14). perl, because
# macOS has no timeout(1).
runs=${ZUG_TEST_RUNS:-1}
limit=${ZUG_TEST_TIMEOUT:-600}
i=1
while [ "$i" -le "$runs" ]; do
  [ "$runs" -gt 1 ] && echo "run $i of $runs"
  perl -e '
    my $t = shift;
    my $pid = fork;
    die "fork: $!" unless defined $pid;
    if ($pid == 0) { exec @ARGV or exit 127 }
    $SIG{ALRM} = sub {
      kill 9, $pid;
      print STDERR "upstream doctests: no exit after $t s, killed (a hang)\n";
      exit 124;
    };
    alarm $t;
    waitpid $pid, 0;
    exit($? & 127 ? 128 + ($? & 127) : $? >> 8);
  ' "$limit" "$exe" --no-intro=true --duration=false
  i=$((i + 1))
done
