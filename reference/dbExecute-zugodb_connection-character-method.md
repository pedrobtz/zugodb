# Run a statement

Runs SQL that returns no result set, such as `CREATE TABLE` or `INSERT`,
and returns the number of rows it changed.

## Usage

``` r
# S4 method for class 'zugodb_connection,character'
dbExecute(conn, statement, ..., params = NULL)
```

## Arguments

- conn:

  A `zugodb_connection`.

- statement:

  A single SQL string; several statements separated by `;` run in order.

- ...:

  Not used.

- params:

  Not supported: the engine has no prepared statements, so a non-`NULL`
  value is a `zug_unsupported` error.

## Value

The rows inserted, updated or deleted, as a number; 0 for any other
statement.
