# dbFetch(n) by chunks (roadmap Stage 3): slices of n - 1, n and n + 1 rows
# concatenate to dbGetQuery()'s result, for a row count n divides and one
# it does not.

test_that("chunks of n - 1, n and n + 1 rows concatenate to the whole", {
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  for (total in c(2500, 2048)) {
    sql <- sprintf(
      "SELECT CAST(i AS INTEGER) AS n, CAST(i AS VARCHAR) AS s, i %% 2 = 0 AS b
       FROM generate_series(1, %d) AS t(i) ORDER BY n",
      total
    )
    whole <- DBI::dbGetQuery(con, sql)
    expect_identical(nrow(whole), as.integer(total))
    for (n in c(1023, 1024, 1025)) {
      got <- fetch_all_by(con, sql, n)
      expect_identical(got$df, whole, label = sprintf("%d rows by %d", total, n))
      expect_identical(got$count, as.double(total))
      # Every slice but the last is n rows.
      expect_true(all(head(got$parts, -1L) == n))
      expect_identical(sum(got$parts), as.integer(total))
    }
  }
})

test_that("n = 0, n = -1, n = Inf and n past the end", {
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  sql <- "SELECT CAST(i AS INTEGER) AS n FROM generate_series(1, 5) AS t(i) ORDER BY n"
  res <- DBI::dbSendQuery(con, sql)
  zero <- DBI::dbFetch(res, 0)
  expect_identical(zero, data.frame(n = integer()))
  expect_identical(DBI::dbGetRowCount(res), 0)
  expect_identical(DBI::dbFetch(res, 2)$n, 1:2)
  expect_identical(DBI::dbFetch(res, 100)$n, 3:5)
  expect_true(DBI::dbHasCompleted(res))
  expect_identical(DBI::dbFetch(res)$n, integer())
  DBI::dbClearResult(res)
  for (all in list(-1, Inf, NA, NA_integer_, NA_real_)) {
    res <- DBI::dbSendQuery(con, sql)
    expect_identical(DBI::dbFetch(res, all)$n, 1:5)
    DBI::dbClearResult(res)
  }
})

test_that("a bad n is refused and fetches nothing", {
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  res <- DBI::dbSendQuery(con, "SELECT 1 AS a")
  withr::defer(DBI::dbClearResult(res))
  for (bad in list(-2, 1.5, NaN, "1", NA_character_, c(1, 2), numeric(), -Inf)) {
    expect_error(DBI::dbFetch(res, bad), class = "zug_argument_error")
  }
  expect_identical(DBI::dbGetRowCount(res), 0)
  expect_identical(DBI::dbFetch(res)$a, 1L)
})

test_that("dbColumnInfo() names each column's R and SQL type", {
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  res <- DBI::dbSendQuery(
    con,
    "SELECT 1 AS i, CAST(1 AS BIGINT) AS g, 1.5 AS d, 'x' AS s, true AS b"
  )
  withr::defer(DBI::dbClearResult(res))
  info <- DBI::dbColumnInfo(res)
  expect_identical(info$name, c("i", "g", "d", "s", "b"))
  # The R type each arrives as, checked against the fetched columns.
  df <- DBI::dbFetch(res)
  got <- vapply(df, function(x) if (is.double(x)) "numeric" else class(x)[1], "")
  expect_identical(info$type, unname(got))
  expect_identical(info$.sql_type, c("INTEGER", "BIGINT", "DOUBLE", "VARCHAR", "BOOLEAN"))
})

test_that("dbSendStatement() reports rows affected and has no rows", {
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  DBI::dbExecute(con, "CREATE TABLE t (x INTEGER)")
  res <- DBI::dbSendStatement(con, "INSERT INTO t VALUES (1), (2), (3)")
  expect_identical(DBI::dbGetRowsAffected(res), 3)
  expect_true(DBI::dbHasCompleted(res))
  expect_identical(DBI::dbGetStatement(res), "INSERT INTO t VALUES (1), (2), (3)")
  expect_warning(df <- DBI::dbFetch(res), "no rows")
  expect_identical(ncol(df), 0L)
  DBI::dbClearResult(res)
  q <- DBI::dbSendQuery(con, "SELECT x FROM t")
  expect_identical(DBI::dbGetRowsAffected(q), 0)
  DBI::dbClearResult(q)
})

test_that("a query whose one column is a BIGINT named Count is a query", {
  # Stage 2's dbExecute() read any one-row BIGINT "Count" result as rows
  # affected, so this returned 5; only after DML is it that.
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  sql <- "SELECT CAST(5 AS BIGINT) AS Count"
  expect_identical(DBI::dbExecute(con, sql), 0)
  expect_identical(DBI::dbGetQuery(con, sql)$Count, 5)
  DBI::dbExecute(con, "CREATE TABLE t (x INTEGER)")
  expect_identical(DBI::dbExecute(con, "INSERT INTO t VALUES (1), (2)"), 2)
})

test_that("CREATE TABLE reports the rows it made, as the engine's count result says", {
  # libzugodb 0.3.0's QueryResult::is_count: INSERT, UPDATE, DELETE and
  # CREATE TABLE [AS] return a count; DuckDB's dbExecute() returns the same.
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  expect_identical(DBI::dbExecute(con, "CREATE TABLE t (x INTEGER)"), 0)
  expect_identical(DBI::dbExecute(con, "INSERT INTO t VALUES (1), (2), (3)"), 3)
  expect_identical(DBI::dbExecute(con, "CREATE TABLE u AS SELECT x FROM t WHERE x > 1"), 2)
  expect_identical(DBI::dbExecute(con, "CREATE TABLE e AS SELECT x FROM t WHERE x > 9"), 0)
  expect_identical(DBI::dbExecute(con, "UPDATE t SET x = x + 1 WHERE x < 3"), 2)
  expect_identical(DBI::dbExecute(con, "DELETE FROM t"), 3)
  res <- DBI::dbSendStatement(con, "CREATE TABLE v AS SELECT * FROM u")
  expect_identical(DBI::dbGetRowsAffected(res), 2)
  expect_true(DBI::dbHasCompleted(res))
  DBI::dbClearResult(res)
  # A query whose answer looks like a count is still a query.
  expect_identical(DBI::dbGetQuery(con, "SELECT count(*) AS Count FROM u")$Count, 2)
})
