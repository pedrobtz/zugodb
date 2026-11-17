# Run a statement

Runs SQL that returns no result set, such as `CREATE TABLE` or `INSERT`,
and returns the number of rows it changed.

## Usage

``` r
# S4 method for class 'zuslothdb_connection,character'
dbExecute(conn, statement, ..., params = NULL)
```

## Arguments

- conn:

  A `zuslothdb_connection`.

- statement:

  A single SQL string; several statements separated by `;` run in order.

- ...:

  Not used.

- params:

  Not supported: SlothDB has no prepared statements, so a non-`NULL`
  value is a `zuslothdb_unsupported` error.

## Value

The rows inserted, updated or deleted, as a number; 0 for any other
statement.
