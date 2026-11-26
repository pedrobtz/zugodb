# zugodb: A Small Embedded SQL Engine with a 'DBI' Driver

A 'DBI' driver for 'libzugodb'
(<https://github.com/pedrobtz/libzugodb>), a small columnar SQL engine
derived from 'SlothDB' (<https://github.com/SouravRoy-ETL/slothdb>),
whose C++ source is bundled and compiled when the package is installed.
SQL queries over in-memory or file-backed databases then need no system
library, no server and no download. The engine is experimental, and this
package is not a replacement for 'duckdb': it is offline only, and a
database file is tied to the engine commit the package bundles.

## See also

Useful links:

- <https://pedrobtz.github.io/zugodb/>

- <https://github.com/pedrobtz/zugodb>

- Report bugs at <https://github.com/pedrobtz/zugodb/issues>

## Author

**Maintainer**: Pedro Baltazar <pedrobtz@gmail.com> \[copyright holder\]

Authors:

- Pedro Baltazar <pedrobtz@gmail.com> \[copyright holder\]

Other contributors:

- SlothDB Contributors (SlothDB, from which the bundled engine derives)
  \[contributor, copyright holder\]

- Martin Leitner-Ankerl (bundled unordered_dense) \[contributor,
  copyright holder\]
