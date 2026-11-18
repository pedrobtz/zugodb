# dbWriteTable(), dbAppendTable() and dbDataType() (roadmap Stage 5): what
# each class is written as, and every refusal leaving the table as it was
# (table_rows() in helper-results.R).

test_that("a factor is written as VARCHAR and read back as its labels", {
  con <- DBI::dbConnect(zuslothdb())
  withr::defer(DBI::dbDisconnect(con))
  f <- factor(c("b", NA, "a", "b"), levels = c("b", "a", "unused"))
  DBI::dbWriteTable(con, "t", data.frame(f = f))
  res <- DBI::dbSendQuery(con, "SELECT * FROM t")
  expect_identical(DBI::dbColumnInfo(res)$sql_type, "VARCHAR")
  DBI::dbClearResult(res)
  expect_identical(table_rows(con, "t")$f, c("b", NA, "a", "b"))
})

test_that("dbDataType() names the SQL type of each class it writes", {
  con <- DBI::dbConnect(zuslothdb())
  withr::defer(DBI::dbDisconnect(con))
  df <- data.frame(
    l = TRUE,
    i = 1L,
    d = 1.5,
    s = "a",
    f = factor("a"),
    dt = as.Date("2024-01-01"),
    ts = as.POSIXct(0, tz = "UTC")
  )
  df$b <- list(as.raw(1))
  want <- c(
    l = "BOOLEAN",
    i = "INTEGER",
    d = "DOUBLE",
    s = "VARCHAR",
    f = "VARCHAR",
    dt = "DATE",
    ts = "TIMESTAMP",
    b = "BLOB"
  )
  expect_identical(DBI::dbDataType(con, df), want)
  expect_identical(DBI::dbDataType(zuslothdb(), 1L), "INTEGER")
  skip_if_not_installed("bit64")
  expect_identical(DBI::dbDataType(con, bit64::as.integer64(1)), "BIGINT")
})

test_that("a column of a class with no SQL type is a type error, before anything changes", {
  con <- DBI::dbConnect(zuslothdb())
  withr::defer(DBI::dbDisconnect(con))
  DBI::dbWriteTable(con, "t", data.frame(x = 1:3))
  bad <- list(
    complex = data.frame(x = 1:2, z = complex(real = 1:2, imaginary = 0)),
    difftime = data.frame(x = 1:2, z = as.difftime(c(1, 2), units = "secs")),
    list = data.frame(x = 1:2, z = I(list(1, "a")))
  )
  for (k in names(bad)) {
    expect_error(
      DBI::dbWriteTable(con, "t", bad[[k]], overwrite = TRUE),
      class = "zuslothdb_type_error",
      label = k
    )
    expect_error(
      DBI::dbWriteTable(con, "u", bad[[k]]),
      class = "zuslothdb_type_error",
      label = k
    )
    expect_error(
      DBI::dbDataType(con, bad[[k]]$z),
      class = "zuslothdb_type_error"
    )
  }
  expect_identical(table_rows(con, "t")$x, 1:3)
  expect_false(DBI::dbExistsTable(con, "u"))
  # No probe table is left behind by a refused write.
  expect_identical(DBI::dbListTables(con), "t")
})

test_that("a value that does not fit its column is refused before any row is written", {
  con <- DBI::dbConnect(zuslothdb())
  withr::defer(DBI::dbDisconnect(con))
  DBI::dbExecute(
    con,
    "CREATE TABLE t (i INTEGER, s SMALLINT, b BIGINT, d DATE, v VARCHAR)"
  )
  DBI::dbExecute(con, "INSERT INTO t VALUES (1, 1, 1, DATE '2024-01-01', 'a')")
  before <- table_rows(con, "t")
  ok <- data.frame(
    i = 1L,
    s = 1L,
    b = 1,
    d = as.Date("2024-01-02"),
    v = "b"
  )
  n <- 5000L
  big <- ok[rep(1L, n), ]
  rownames(big) <- NULL
  cases <- list(
    fraction = within(big, i <- as.double(i))[, names(ok)],
    out_of_range = big,
    beyond_bigint = big,
    fractional_date = big,
    bad_utf8 = big
  )
  cases$fraction$i[[n]] <- 1.5
  cases$out_of_range$s[[n]] <- 32768L
  cases$beyond_bigint$b[[n]] <- 2^63
  cases$fractional_date$d[[n]] <- structure(19725.5, class = "Date")
  cases$bad_utf8$v[[n]] <- rawToChar(as.raw(c(0x61, 0xff)))
  for (k in names(cases)) {
    err <- expect_error(
      DBI::dbAppendTable(con, "t", cases[[k]]),
      class = "zuslothdb_type_error",
      label = k
    )
    expect_match(conditionMessage(err), "row 5000", fixed = TRUE, label = k)
    expect_identical(table_rows(con, "t"), before, label = k)
  }
  # The edges themselves fit.
  edge <- ok[c(1, 1), ]
  edge$s <- c(-32768L, 32767L)
  edge$b <- c(-2^63, 2^53)
  expect_identical(DBI::dbAppendTable(con, "t", edge), 2)
})

test_that("overwrite and append replace and add; a third write is refused", {
  con <- DBI::dbConnect(zuslothdb())
  withr::defer(DBI::dbDisconnect(con))
  DBI::dbWriteTable(con, "t", data.frame(x = 1:2))
  expect_error(
    DBI::dbWriteTable(con, "t", data.frame(x = 3L)),
    class = "zuslothdb_argument_error"
  )
  DBI::dbWriteTable(con, "t", data.frame(x = 3:4), append = TRUE)
  expect_identical(table_rows(con, "t")$x, 1:4)
  DBI::dbWriteTable(con, "t", data.frame(y = "a"), overwrite = TRUE)
  expect_identical(table_rows(con, "t"), data.frame(y = "a"))
  expect_error(
    DBI::dbWriteTable(
      con,
      "t",
      data.frame(y = "b"),
      overwrite = TRUE,
      append = TRUE
    ),
    class = "zuslothdb_argument_error"
  )
  expect_error(
    DBI::dbWriteTable(con, "u", data.frame(y = 1), temporary = TRUE),
    class = "zuslothdb_unsupported"
  )
  expect_error(
    DBI::dbWriteTable(con, "u", 1:3),
    class = "zuslothdb_argument_error"
  )
})

test_that("an overwrite whose data does not fit keeps the old table", {
  con <- DBI::dbConnect(zuslothdb())
  withr::defer(DBI::dbDisconnect(con))
  DBI::dbWriteTable(con, "t", data.frame(x = 1:3))
  new <- data.frame(x = c(1, 2, 2.5))
  expect_error(
    DBI::dbWriteTable(
      con,
      "t",
      new,
      overwrite = TRUE,
      field.types = c(x = "INTEGER")
    ),
    class = "zuslothdb_type_error"
  )
  expect_identical(table_rows(con, "t")$x, 1:3)
  expect_identical(DBI::dbListTables(con), "t")
})

test_that("field.types and row.names change what is written", {
  con <- DBI::dbConnect(zuslothdb())
  withr::defer(DBI::dbDisconnect(con))
  DBI::dbWriteTable(
    con,
    "t",
    data.frame(x = c(1, 2)),
    field.types = c(x = "INTEGER")
  )
  expect_identical(table_rows(con, "t")$x, 1:2)
  df <- data.frame(x = 1:2, row.names = c("a", "b"))
  DBI::dbWriteTable(con, "r", df, row.names = TRUE)
  expect_identical(table_rows(con, "r")$row_names, c("a", "b"))
  expect_error(
    DBI::dbWriteTable(
      con,
      "u",
      data.frame(x = 1),
      field.types = c(nope = "INTEGER")
    ),
    class = "zuslothdb_argument_error"
  )
  expect_false(DBI::dbExistsTable(con, "u"))
})

test_that("dbAppendTable() matches columns by name and fills the rest with NULL", {
  con <- DBI::dbConnect(zuslothdb())
  withr::defer(DBI::dbDisconnect(con))
  DBI::dbExecute(con, "CREATE TABLE t (a INTEGER, b VARCHAR, c DOUBLE)")
  expect_identical(DBI::dbAppendTable(con, "t", data.frame(c = 2.5, a = 1L)), 1)
  expect_identical(
    table_rows(con, "t"),
    data.frame(a = 1L, b = NA_character_, c = 2.5)
  )
  expect_error(
    DBI::dbAppendTable(con, "t", data.frame(a = 1L, z = 1)),
    class = "zuslothdb_argument_error"
  )
  expect_error(
    DBI::dbAppendTable(
      con,
      "t",
      data.frame(a = 1L, a = 2L, check.names = FALSE)
    ),
    class = "zuslothdb_argument_error"
  )
  expect_identical(DBI::dbAppendTable(con, "t", data.frame(a = integer())), 0)
  expect_identical(nrow(table_rows(con, "t")), 1L)
})
