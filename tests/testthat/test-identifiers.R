# Table and column names that need quoting round-trip through every table
# function (roadmap Stage 5): spaces, a single quote, reserved words, dots,
# a leading digit, UTF-8, and case. Names match ignoring ASCII case and
# keep their spelling, as DuckDB's (libzugodb 0.3.0): "Mixed" and "mixed"
# are one table. A name holding a double quote is refused (below).

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
  expect_error(
    DBI::dbWriteTable(con, "mixed", data.frame(x = 1L)),
    class = "zug_error"
  )
  for (nm in names) {
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

test_that("a name holding a double quote, or NA, is refused", {
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  expect_error(
    DBI::dbQuoteIdentifier(con, "a\"b"),
    class = "zug_argument_error"
  )
  expect_error(
    DBI::dbQuoteIdentifier(con, NA_character_),
    class = "zug_argument_error"
  )
  expect_error(
    DBI::dbWriteTable(con, "a\"b", data.frame(x = 1)),
    class = "zug_argument_error"
  )
  df <- data.frame(1)
  names(df) <- "a\"b"
  expect_error(
    DBI::dbWriteTable(con, "t", df),
    class = "zug_argument_error"
  )
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
