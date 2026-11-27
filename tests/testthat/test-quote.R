# Quoting strings and literals (Stage 9, from DBItest's quote_* cases).
# The oracle is the engine: a quoted value selected back is the value.

test_that("dbQuoteString() doubles ' and gives NULL for NA", {
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  expect_identical(
    DBI::dbQuoteString(con, c(a = "it's", NA, "")),
    DBI::SQL(c("'it''s'", "NULL", "''"), names = c("a", "", ""))
  )
  expect_identical(DBI::dbQuoteString(con, character()), DBI::SQL(character()))
  sql <- DBI::SQL("'x'")
  expect_identical(DBI::dbQuoteString(con, sql), sql)
})

test_that("a quoted string selects back as itself", {
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  values <- c(
    "it's",
    "''",
    "a\\b",
    "\\x41",
    "\"",
    "--",
    "/*",
    ";",
    "é中",
    "tab\there",
    "line\nbreak",
    " "
  )
  q <- DBI::dbQuoteString(con, values)
  sql <- paste0(
    "SELECT ",
    paste0(q, " AS v", seq_along(values), collapse = ", ")
  )
  got <- unlist(DBI::dbGetQuery(con, sql), use.names = FALSE)
  expect_identical(got, enc2utf8(values))
  expect_true(is.na(
    DBI::dbGetQuery(
      con,
      paste("SELECT", DBI::dbQuoteString(con, NA_character_), "AS v")
    )$v
  ))
})

test_that("a quoted blob selects back as its bytes", {
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  # Every byte once, a NUL alone, an empty blob and a NULL.
  values <- list(as.raw(0:255), as.raw(0), raw(0), NULL)
  q <- DBI::dbQuoteLiteral(con, values)
  expect_identical(as.character(q[[4L]]), "NULL")
  sql <- paste0(
    "SELECT ",
    paste0(q[1:3], " AS v", 1:3, collapse = ", ")
  )
  got <- DBI::dbGetQuery(con, sql)
  expect_identical(
    lapply(got, function(x) x[[1L]]),
    list(v1 = values[[1L]], v2 = values[[2L]], v3 = values[[3L]])
  )
  # A fetched column, of the blob class, quotes the same.
  expect_true(inherits(got$v1, "blob"))
  expect_identical(DBI::dbQuoteLiteral(con, got$v1), q[1])
  expect_identical(
    DBI::dbQuoteLiteral(con, list(as.raw(c(1, 171)))),
    DBI::SQL("CAST('\\x01\\xab' AS BLOB)")
  )
})

test_that("other literals are quoted as DBI quotes them", {
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  x <- list(1L, 2.5, "it's", TRUE, as.Date("2020-02-29"))
  for (v in x) {
    expect_identical(
      DBI::dbQuoteLiteral(con, v),
      methods::getMethod("dbQuoteLiteral", "DBIConnection")(con, v)
    )
  }
})
