# 64-bit integers at 2^53 under each bigint (design §6; roadmap Stage 3).
# The oracle is each literal's decimal string: "character" gives it
# exactly, "integer64" must equal it, and "numeric" gives the same number
# up to 2^53 and a zug_type_error past it, never a rounded value.

test_that("bigint = 'character' gives every value's exact digits", {
  con <- DBI::dbConnect(zugodb(), bigint = "character")
  withr::defer(DBI::dbDisconnect(con))
  lits <- c(
    "9007199254740991", "9007199254740992", "9007199254740993",
    "-9007199254740991", "-9007199254740992", "-9007199254740993",
    "9223372036854775807", "0"
  )
  expect_identical(bigint_query(con, lits), lits)
  expect_identical(
    DBI::dbGetQuery(con, "SELECT CAST(NULL AS BIGINT) AS v")$v,
    NA_character_
  )
})

test_that("bigint = 'numeric' is exact to 2^53 and refuses past it", {
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  inside <- c("9007199254740991", "9007199254740992", "-9007199254740991",
              "-9007199254740992")
  got <- bigint_query(con, inside)
  expect_identical(got, as.numeric(inside))
  expect_identical(format(got, scientific = FALSE, digits = 22, trim = TRUE), inside)
  for (lit in c("9007199254740993", "-9007199254740993", "9223372036854775807")) {
    expect_error(
      DBI::dbGetQuery(con, sprintf("SELECT CAST(%s AS BIGINT) AS v", lit)),
      class = "zug_type_error"
    )
  }
})

test_that("a refused value fetches nothing, and names its column and row", {
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  res <- DBI::dbSendQuery(
    con,
    "SELECT v FROM (VALUES (1, CAST(1 AS BIGINT)), (2, CAST(9007199254740993 AS BIGINT))) AS t(i, v) ORDER BY i"
  )
  withr::defer(DBI::dbClearResult(res))
  err <- expect_error(DBI::dbFetch(res), class = "zug_type_error")
  expect_match(conditionMessage(err), "`v`.*row 2")
  expect_identical(DBI::dbGetRowCount(res), 0)
  # The rows before it can still be fetched one by one.
  expect_identical(DBI::dbFetch(res, 1)$v, 1)
})

test_that("bigint = 'integer64' is exact, and -2^63 (its NA) is refused", {
  skip_if_not_installed("bit64")
  con <- DBI::dbConnect(zugodb(), bigint = "integer64")
  withr::defer(DBI::dbDisconnect(con))
  lits <- c("9007199254740991", "9007199254740992", "9007199254740993",
            "-9007199254740993", "9223372036854775807", "-9223372036854775807")
  got <- bigint_query(con, lits)
  expect_s3_class(got, "integer64")
  expect_identical(bit64::as.character.integer64(got), lits)
  expect_true(bit64::is.na.integer64(
    DBI::dbGetQuery(con, "SELECT CAST(NULL AS BIGINT) AS v")$v
  ))
  expect_error(
    DBI::dbGetQuery(con, "SELECT CAST(-9223372036854775807 - 1 AS BIGINT) AS v"),
    class = "zug_type_error"
  )
})

test_that("bigint must be one of the three", {
  for (bad in list("double", NA_character_, c("numeric", "character"), 1)) {
    expect_error(
      DBI::dbConnect(zugodb(), bigint = bad),
      class = "zug_argument_error"
    )
  }
})
