# Write Arrow C streams into tables

[`DBI::dbWriteTableArrow()`](https://dbi.r-dbi.org/reference/dbWriteTableArrow.html)
creates a table from an Arrow C stream, or replaces one
(`overwrite = TRUE`) or adds to one (`append = TRUE`);
[`DBI::dbAppendTableArrow()`](https://dbi.r-dbi.org/reference/dbAppendTableArrow.html)
adds a stream's rows to a table that exists;
[`DBI::dbCreateTableArrow()`](https://dbi.r-dbi.org/reference/dbCreateTableArrow.html)
creates an empty table from an Arrow schema. Any producer of the Arrow C
stream interface works: a stream handle of class
`nanoarrow_array_stream`, as `zuarrow`, `nanoarrow`, `arrow` and 'ADBC'
drivers make, or a data frame, which
[`zuarrow::arrow_stream()`](https://pedrobtz.github.io/zuarrow/reference/arrow_array.html)
converts (`zuarrow` is then needed). The stream is read once, one batch
at a time, and released: it cannot be read again.

## Usage

``` r
# S4 method for class 'zugodb_connection'
dbWriteTableArrow(
  conn,
  name,
  value,
  ...,
  overwrite = FALSE,
  append = FALSE,
  temporary = FALSE
)

# S4 method for class 'zugodb_connection'
dbAppendTableArrow(conn, name, value, ...)

# S4 method for class 'zugodb_connection'
dbCreateTableArrow(conn, name, value, ..., temporary = FALSE)
```

## Arguments

- conn:

  A `zugodb_connection`.

- name:

  The table's name, as for
  [`DBI::dbWriteTable()`](https://dbi.r-dbi.org/reference/dbWriteTable.html).

- value:

  For `dbWriteTableArrow()` and `dbAppendTableArrow()`, an Arrow C
  stream handle (class `nanoarrow_array_stream`) or a data frame. For
  `dbCreateTableArrow()`, a schema handle (class `nanoarrow_schema`), or
  a stream handle, data frame or format string, which
  [`zuarrow::arrow_schema()`](https://pedrobtz.github.io/zuarrow/reference/arrow_schema.html)
  reads a schema from.

- ...:

  Not used.

- overwrite, append:

  `TRUE` to replace or to add to a table that exists; at most one of
  them.

- temporary:

  Not supported: the engine has no temporary tables.

## Value

`dbWriteTableArrow()`, `dbCreateTableArrow()`: `TRUE`, invisibly.
`dbAppendTableArrow()`: the number of rows written.

## Details

A new table's columns take these engine types:

|  |  |
|----|----|
| Arrow | Engine |
| boolean | `BOOLEAN` |
| int8 | `TINYINT` |
| int16, uint8 | `SMALLINT` |
| int32, uint16 | `INTEGER` |
| int64, uint32, uint64 | `BIGINT` |
| float, double | `FLOAT`, `DOUBLE` |
| string, large string, string view | `VARCHAR` |
| binary, large binary, binary view, fixed-size binary | `BLOB` |
| date32, date64 | `DATE` |
| timestamp, any unit and time zone | `TIMESTAMP`, the instant in UTC |

Another Arrow type (a decimal, a time of day, a duration, an interval, a
list, a struct, a map, a union, a dictionary, an extension type, the
null type, a half float) is a `zug_type_error` naming the column, raised
from the stream's schema before any row is read. So is a value its
column cannot hold: a uint64 past 2^63 - 1, a timestamp with a fraction
of a microsecond, a string that is not valid UTF-8 or holds a NUL. An
append matches columns by name, as
[`DBI::dbAppendTable()`](https://dbi.r-dbi.org/reference/dbAppendTable.html)
does, and converts as it does (a whole double into an `INTEGER`, say).

A load that fails part-way, by such a value, by the producer's own error
(a `zug_stream_error` carrying its message) or by an interrupt, leaves
nothing: a new table is removed, and an append or an overwrite reads the
stream into a staging table first, so that the table it writes is only
changed once the whole stream has been read. The engine has no
transactions, so this costs a copy of the rows.

## Examples

``` r
con <- DBI::dbConnect(zugodb())
stream <- zuarrow::arrow_stream(head(mtcars))
DBI::dbWriteTableArrow(con, "cars", stream)
DBI::dbAppendTableArrow(con, "cars", tail(mtcars))
#> [1] 6
DBI::dbGetQuery(con, "SELECT count(*) AS n FROM cars")
#>    n
#> 1 12
DBI::dbDisconnect(con)
```
