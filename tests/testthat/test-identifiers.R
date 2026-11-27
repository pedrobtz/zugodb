# Table and column names that need quoting round-trip through every table
# function (roadmap Stage 5): spaces, a single quote, reserved words, dots,
# a leading digit, UTF-8, and case. Names match ignoring ASCII case and
# keep their spelling, as DuckDB's (libzugodb 0.3.0): "Mixed" and "mixed"
# are one table. A double quote in a name is doubled (below).

test_that("names that need quoting round-trip through every table function", {
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  names <- c(
    "with space",
    "it's",
    "select",
    "a.b",
    "1lead",
    "été 日",
    "Mixed",
    "x;y"
  )
  for (nm in names) {
    df <- data.frame(1L, "v")
    names(df) <- c(nm, paste(nm, "2"))
    DBI::dbWriteTable(con, nm, df)
    expect_true(DBI::dbExistsTable(con, nm), label = nm)
    expect_identical(DBI::dbListFields(con, nm), names(df), label = nm)
    expect_identical(
      DBI::dbReadTable(con, nm, check.names = FALSE),
      df,
      label = nm
    )
    DBI::dbAppendTable(con, nm, df)
    expect_identical(
      nrow(DBI::dbReadTable(con, nm, check.names = FALSE)),
      2L,
      label = nm
    )
  }
  expect_setequal(DBI::dbListTables(con), names)
  # Case is kept in the name and ignored in a match: "mixed" is "Mixed".
  expect_true("Mixed" %in% DBI::dbListTables(con))
  expect_identical(DBI::dbReadTable(con, "MIXED")$Mixed, c(1L, 1L))
  # Every method matches as the engine does (dbExistsTable() and
  # dbListFields() matched the exact spelling until Stage 9).
  expect_true(DBI::dbExistsTable(con, "MIXED"))
  expect_true(DBI::dbExistsTable(con, DBI::Id(table = "mIxEd")))
  expect_identical(DBI::dbListFields(con, "MIXED"), c("Mixed", "Mixed 2"))
  expect_error(
    DBI::dbWriteTable(con, "mixed", data.frame(x = 1L)),
    class = "zug_error"
  )
  DBI::dbWriteTable(con, "MIXED", data.frame(y = 2L), overwrite = TRUE)
  expect_identical(DBI::dbGetQuery(con, "SELECT y FROM mixed")$y, 2L)
  expect_identical(sum(ascii_lower(DBI::dbListTables(con)) == "mixed"), 1L)
  DBI::dbRemoveTable(con, "mixed")
  expect_false(DBI::dbExistsTable(con, "Mixed"))
  for (nm in setdiff(names, "Mixed")) {
    DBI::dbRemoveTable(con, nm)
  }
  expect_identical(DBI::dbListTables(con), character())
})

test_that("a name is given as a string, Id(table =) or quoted SQL", {
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  DBI::dbWriteTable(con, DBI::Id(table = "my t"), data.frame(x = 1L))
  expect_true(DBI::dbExistsTable(con, "my t"))
  expect_true(DBI::dbExistsTable(con, DBI::SQL("\"my t\"")))
  expect_identical(DBI::dbListFields(con, DBI::Id(table = "my t")), "x")
  expect_identical(
    DBI::dbQuoteIdentifier(con, c(a = "my t")),
    DBI::SQL("\"my t\"", names = "a")
  )
  expect_error(
    DBI::dbWriteTable(
      con,
      DBI::Id(schema = "s", table = "t"),
      data.frame(x = 1)
    ),
    class = "zug_unsupported"
  )
})

test_that("a double quote in a name is doubled, and NA is refused", {
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  expect_identical(
    DBI::dbQuoteIdentifier(con, c(a = "a\"b", "\"", "x\"\"y")),
    DBI::SQL(
      c("\"a\"\"b\"", "\"\"\"\"", "\"x\"\"\"\"y\""),
      names = c("a", "", "")
    )
  )
  expect_identical(
    DBI::dbQuoteIdentifier(con, character()),
    DBI::SQL(character())
  )
  sql <- DBI::SQL("\"a\"\"b\"")
  expect_identical(DBI::dbQuoteIdentifier(con, sql), sql)
  expect_error(
    DBI::dbQuoteIdentifier(con, NA_character_),
    class = "zug_argument_error"
  )
  expect_identical(DBI::dbListTables(con), character())
})

test_that("a table and a column named with double quotes round-trip", {
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  df <- data.frame(1:2, c("p", "q"))
  names(df) <- c("x\"y", "\"")
  DBI::dbWriteTable(con, "a\"b", df)
  # The engine's own view: SHOW TABLES and a query naming the table in SQL.
  expect_identical(DBI::dbGetQuery(con, "SHOW TABLES")[[1L]], "a\"b")
  expect_identical(
    DBI::dbGetQuery(con, "SELECT \"x\"\"y\" AS v FROM \"a\"\"b\" ORDER BY v")$v,
    1:2
  )
  expect_identical(DBI::dbListFields(con, "a\"b"), names(df))
  expect_identical(DBI::dbReadTable(con, "a\"b", check.names = FALSE), df)
  expect_true(DBI::dbExistsTable(con, "a\"b"))
  DBI::dbAppendTable(con, "a\"b", df)
  expect_identical(nrow(DBI::dbReadTable(con, "a\"b")), 4L)
  DBI::dbWriteTable(con, "a\"b", df, overwrite = TRUE)
  expect_identical(nrow(DBI::dbReadTable(con, "a\"b")), 2L)
  DBI::dbRemoveTable(con, "a\"b")
  expect_identical(DBI::dbListTables(con), character())
})

test_that("removing a missing table is an error unless asked otherwise", {
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  expect_error(
    DBI::dbRemoveTable(con, "nope"),
    class = "zug_argument_error"
  )
  expect_invisible(DBI::dbRemoveTable(con, "nope", fail_if_missing = FALSE))
  expect_error(
    DBI::dbListFields(con, "nope"),
    class = "zug_argument_error"
  )
  expect_false(DBI::dbExistsTable(con, "nope"))
})
