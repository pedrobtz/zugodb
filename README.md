
# zugodb

<!-- badges: start -->
[![R-CMD-check](https://github.com/pedrobtz/zugodb/actions/workflows/R-CMD-check.yaml/badge.svg)](https://github.com/pedrobtz/zugodb/actions/workflows/R-CMD-check.yaml)
[![coverage](https://raw.githubusercontent.com/pedrobtz/zugodb/gh-pages/badges/coverage.svg)](https://github.com/pedrobtz/zugodb/actions/workflows/coverage.yaml)
<!-- badges: end -->

zugodb is to [libzugodb](https://github.com/pedrobtz/libzugodb) what
duckdb is to DuckDB. libzugodb is a small columnar SQL engine in C++20,
derived from [SlothDB](https://github.com/SouravRoy-ETL/slothdb); its
source is bundled into the package and compiled when you install it. On
top of it sits a DBI driver, so SQL works with no system library, no
server and no download. The engine is experimental, and so is this
package: it is not a duckdb replacement.

The package is in development. Nothing of the engine is built yet; the
[roadmap](https://github.com/pedrobtz/zugodb/blob/main/.agents/roadmap.md)
takes it to its first release.

## Installation

You can install the development version of zugodb from
[GitHub](https://github.com/pedrobtz/zugodb) with:

``` r
# install.packages("pak")
pak::pak("pedrobtz/zugodb")
```
