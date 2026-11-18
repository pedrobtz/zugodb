
# zuslothdb

<!-- badges: start -->
[![R-CMD-check](https://github.com/pedrobtz/zuslothdb/actions/workflows/R-CMD-check.yaml/badge.svg)](https://github.com/pedrobtz/zuslothdb/actions/workflows/R-CMD-check.yaml)
[![coverage](https://raw.githubusercontent.com/pedrobtz/zuslothdb/gh-pages/badges/coverage.svg)](https://github.com/pedrobtz/zuslothdb/actions/workflows/coverage.yaml)
<!-- badges: end -->

zuslothdb is to [SlothDB](https://github.com/SouravRoy-ETL/slothdb) what
duckdb is to DuckDB. The engine's C++ source is bundled into the package
and compiled when you install it. On top of it sits a DBI driver, so SQL
over local CSV, Parquet and JSON files works with no system
library, no server and no download. SlothDB calls itself experimental,
and so is this package: it is not a duckdb replacement.

The package is in development. Nothing of the engine is built yet; the
[roadmap](https://github.com/pedrobtz/zuslothdb/blob/main/.agents/roadmap.md)
takes it to its first release.

## Installation

You can install the development version of zuslothdb from
[GitHub](https://github.com/pedrobtz/zuslothdb) with:

``` r
# install.packages("pak")
pak::pak("pedrobtz/zuslothdb")
```
