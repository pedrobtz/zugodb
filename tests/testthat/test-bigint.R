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

test_that("bigint = 'integer64' loads bit64, so its values convert", {
  # Without bit64's methods, an integer64 is a double's bits: as.integer()
  # gave 0 for 10^10 and format() 4.94e-314 (DBItest's
  # connect_bigint_integer64). The child R has not loaded bit64.
  skip_if_not_installed("bit64")
  pkg <- find.package("zugodb")
  skip_if_not(
    file.exists(file.path(pkg, "Meta", "package.rds")),
    "needs the installed package: the reader is a child R"
  )
  dir <- withr::local_tempdir()
  out <- normalizePath(file.path(dir, "out.rds"), "/", mustWork = FALSE)
  script <- file.path(dir, "child.R")
  writeLines(
    c(
      "args <- commandArgs(trailingOnly = TRUE)",
      "library(zugodb, lib.loc = args[[2]])",
      "before <- isNamespaceLoaded('bit64')",
      "con <- DBI::dbConnect(zugodb(), bigint = 'integer64')",
      "v <- DBI::dbGetQuery(con, 'SELECT CAST(10000000000 AS BIGINT) AS a')$a",
      "w <- NULL",
      "i <- withCallingHandlers(as.integer(v), warning = function(e) {",
      "  w <<- conditionMessage(e); invokeRestart('muffleWarning') })",
      "saveRDS(list(before = before, after = isNamespaceLoaded('bit64'),",
      "  i = i, warned = !is.null(w), d = as.numeric(v), s = as.character(v)),",
      "  args[[1]])"
    ),
    script
  )
  res <- run_child_r(script, c(out, dirname(pkg)), timeout = 300)
  expect_null(
    attr(res, "status"),
    info = paste(c("child R printed:", res), collapse = "\n")
  )
  got <- readRDS(out)
  expect_false(got$before)
  expect_true(got$after)
  expect_identical(got$i, NA_integer_)
  expect_true(got$warned)
  expect_identical(got$d, 1e10)
  expect_identical(got$s, "10000000000")
})

test_that("bigint = 'integer64' without bit64 is a zug_argument_error", {
  local_mocked_bindings(has_bit64 = function() FALSE)
  expect_error(
    DBI::dbConnect(zugodb(), bigint = "integer64"),
    class = "zug_argument_error"
  )
})
