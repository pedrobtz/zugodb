# VARCHAR (design §6; roadmap Stage 3): strings arrive marked UTF-8 and
# identical to the literal sent, multi-byte text included; a string R
# cannot hold is a zuslothdb_type_error, never a replaced or cut one.

test_that("multi-byte text arrives identical to the literal, marked UTF-8", {
  con <- DBI::dbConnect(zuslothdb())
  withr::defer(DBI::dbDisconnect(con))
  texts <- c("héllo", "漢字かな", "🙂👍", "Ωμέγα", "a b", "tab\there")
  for (x in texts) {
    lit <- gsub("'", "''", x, fixed = TRUE)
    got <- DBI::dbGetQuery(con, paste0("SELECT '", lit, "' AS s"))$s
    expect_identical(got, enc2utf8(x))
    # R marks only non-ASCII strings; ASCII ones stay "unknown".
    expect_identical(Encoding(got), if (grepl("[^\\x01-\\x7F]", x, perl = TRUE)) "UTF-8" else "unknown")
    expect_identical(nchar(got, "bytes"), nchar(enc2utf8(x), "bytes"))
  }
})

test_that("the same under LC_ALL=C and a hostile set of options", {
  withr::local_locale(c(LC_CTYPE = "C", LC_COLLATE = "C"))
  withr::local_options(OutDec = ",", scipen = -5, digits = 3)
  con <- DBI::dbConnect(zuslothdb())
  withr::defer(DBI::dbDisconnect(con))
  got <- DBI::dbGetQuery(con, "SELECT '漢字' AS s, 1234567.5 AS d, 12345678 AS i")
  expect_identical(got$s, "漢字")
  expect_identical(got$d, 1234567.5)
  expect_identical(got$i, 12345678L)
})

test_that("an invalid UTF-8 string is refused, not passed on", {
  # SlothDB's substr() cuts bytes (oracle/known-differences.md).
  con <- DBI::dbConnect(zuslothdb())
  withr::defer(DBI::dbDisconnect(con))
  err <- expect_error(
    DBI::dbGetQuery(con, "SELECT substr('é', 1, 1) AS s"),
    class = "zuslothdb_type_error"
  )
  expect_match(conditionMessage(err), "not valid UTF-8")
  expect_error(
    DBI::dbGetQuery(con, "SELECT chr(0) AS s"),
    class = "zuslothdb_type_error"
  )
})

test_that("a statement with non-ASCII text runs as sent", {
  con <- DBI::dbConnect(zuslothdb())
  withr::defer(DBI::dbDisconnect(con))
  DBI::dbExecute(con, "CREATE TABLE t (s VARCHAR)")
  latin1 <- iconv("café", "UTF-8", "latin1")
  expect_identical(Encoding(latin1), "latin1")
  DBI::dbExecute(con, paste0("INSERT INTO t VALUES ('", latin1, "')"))
  expect_identical(DBI::dbGetQuery(con, "SELECT s FROM t")$s, "café")
})
