# Information about the bundled engine

Reports which engine source the package was built from: a tagged release
of libzugodb, the package's engine, which descends from SlothDB.

## Usage

``` r
zug_info()
```

## Value

A list with elements `engine` (`"libzugodb"`), `repository`, `commit`
and `tag` (libzugodb's repository, the full commit hash and its release
tag), `origin` and `origin_commit` (SlothDB and the commit libzugodb
descends from), `describe` (the last SlothDB release before it, as
`"v0.2.7+"`) and `threads` (the engine's thread cap, an integer).

## Examples

``` r
info <- zug_info()
info$commit
#> [1] "9df82d5a03fb53c802f3633c5dafb9841b226b98"
info$tag
#> [1] "v0.4.1"
```
