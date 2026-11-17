# Information about the bundled engine

Reports which SlothDB source the package was built from and how it was
changed: the upstream repository and commit, and the patches applied to
it, in order. A database file is tied to this commit.

## Usage

``` r
zuslothdb_info()
```

## Value

A list with elements `engine` (`"SlothDB"`), `repository`, `commit` (the
full upstream commit hash), `describe` (the last upstream release before
it, as `"v0.2.7+"`), `patches` (a character vector of patch file names,
in the order they are applied) and `threads` (the engine's thread cap,
an integer).

## Examples

``` r
info <- zuslothdb_info()
info$commit
#> [1] "8180f215847015944c935f56f008d194e2c1494c"
info$patches
#> [1] "0001-mingw.patch"             "0002-no-process-output.patch"
#> [3] "0003-offline.patch"           "0004-threads.patch"          
#> [5] "0005-strftime-arity.patch"    "0006-compiler-warnings.patch"
#> [7] "0007-read-only.patch"        
```
