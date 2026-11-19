# Information about the bundled engine

Reports which engine source the package was built from: a tagged release
of libzugodb, the package's engine, which descends from SlothDB.

## Usage

``` r
zuslothdb_info()
```

## Value

A list with elements `engine` (`"libzugodb"`), `repository`, `commit`
and `tag` (libzugodb's repository, the full commit hash and its release
tag), `origin` and `origin_commit` (SlothDB and the commit libzugodb
descends from), `describe` (the last SlothDB release before it, as
`"v0.2.7+"`) and `threads` (the engine's thread cap, an integer).

## Examples

``` r
info <- zuslothdb_info()
info$commit
#> [1] "d3ec8c5ff1818c5079ecac0185514656fb715e66"
info$tag
#> [1] "v0.1.1"
```
