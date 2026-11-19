# Disconnect from a zugodb database

Closes the connection. With `shutdown = TRUE`, the default, the database
closes too once this was its last open connection, and a file-backed
database is written to its file. The driver's next
[`DBI::dbConnect()`](https://dbi.r-dbi.org/reference/dbConnect.html)
opens it again.

## Usage

``` r
# S4 method for class 'zugodb_connection'
dbDisconnect(conn, shutdown = TRUE, ...)
```

## Arguments

- conn:

  A `zugodb_connection`.

- shutdown:

  `FALSE` to keep the database open, for the driver's next connection,
  after its last connection closes. Other open connections to the
  database are never closed by this.

- ...:

  Not used.

## Value

`TRUE`, invisibly; with a warning and `FALSE` if the connection was
already closed.
