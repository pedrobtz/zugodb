# Conditions (design §5): every error is a zug_error; the engine's own
# carry its message.

test_that("an engine error is a zug_sql_error with the engine's message", {
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  err <- expect_error(
    DBI::dbExecute(con, "INSERT INTO no_such_table VALUES (1)"),
    class = "zug_sql_error"
  )
  expect_s3_class(err, "zug_error")
  # The engine's message names the table; the glue adds nothing to it.
  expect_match(conditionMessage(err), "no_such_table", fixed = TRUE)
  expect_error(DBI::dbExecute(con, "SELEC 1"), class = "zug_sql_error")
})

test_that("params is zug_unsupported: SlothDB has no prepared statements", {
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  DBI::dbExecute(con, "CREATE TABLE t (x INTEGER)")
  expect_error(
    DBI::dbExecute(con, "INSERT INTO t VALUES (?)", params = list(1L)),
    class = "zug_unsupported"
  )
})

test_that("the statement must be one string", {
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  for (bad in list(NA_character_, c("SELECT 1", "SELECT 2"), character())) {
    expect_error(DBI::dbExecute(con, bad), class = "zug_argument_error")
  }
})

test_that("params is unsupported on every route that takes it", {
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  DBI::dbExecute(con, "CREATE TABLE t (x INTEGER)")
  expect_error(
    DBI::dbSendQuery(con, "SELECT ? AS a", params = list(1L)),
    class = "zug_unsupported"
  )
  expect_error(
    DBI::dbGetQuery(con, "SELECT ? AS a", params = list(1L)),
    class = "zug_unsupported"
  )
  expect_error(
    DBI::dbSendStatement(con, "INSERT INTO t VALUES (?)", params = list(1L)),
    class = "zug_unsupported"
  )
  res <- DBI::dbSendQuery(con, "SELECT 1 AS a")
  withr::defer(DBI::dbClearResult(res))
  expect_error(DBI::dbBind(res, list(1L)), class = "zug_unsupported")
})

test_that("TIME is a type error naming the column; HUGEINT, INTERVAL and UUID are refused", {
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  DBI::dbExecute(con, "CREATE TABLE t (tm TIME)")
  DBI::dbExecute(con, "INSERT INTO t VALUES (CAST('01:00:00' AS TIME))")
  # TIME: the engine's values are right since libzugodb 0.3.0, and zugodb
  # does not read them yet (#43).
  for (bigint in c("numeric", "integer64", "character")) {
    con2 <- DBI::dbConnect(zugodb(), bigint = bigint)
    err <- expect_error(
      DBI::dbGetQuery(con2, "SELECT TIME '01:00:00' AS tm_col"),
      class = "zug_type_error"
    )
    expect_match(conditionMessage(err), "`tm_col`", fixed = TRUE)
    expect_match(conditionMessage(err), "TIME", fixed = TRUE)
    DBI::dbDisconnect(con2)
  }
  err <- expect_error(DBI::dbGetQuery(con, "SELECT tm FROM t"), class = "zug_type_error")
  expect_match(conditionMessage(err), "`tm`", fixed = TRUE)
  # The engine refuses the types it cannot hold a value of (libzugodb 0.2.0).
  for (type in c("HUGEINT", "INTERVAL", "UUID")) {
    for (sql in c(
      sprintf("CREATE TABLE u (x %s)", type),
      sprintf("SELECT CAST(NULL AS %s) AS x", type)
    )) {
      err <- expect_error(DBI::dbExecute(con, sql), class = "zug_sql_error", label = sql)
      expect_match(conditionMessage(err), paste(type, "is not supported"), fixed = TRUE)
    }
  }
  expect_false(DBI::dbExistsTable(con, "u"))
})

test_that("the parallel window sort answers as the serial one does", {
  # The engine's one worker-thread phase (libzugodb 0.4.0): a window's
  # partitions are sorted on several workers from 200,000 rows and two
  # partitions. An exception there becomes the query's error, rethrown
  # after the join (libzugodb 0.2.0, the assessment's A1); no query this
  # package can send throws on that path, since the sort keys are computed
  # before it, so the fork's doctest (ArmWorkerFault) carries the
  # exception and this test the answer at caps 1 and 4.
  skip_if_heavy()
  n <- 240000L
  value <- data.frame(k = seq_len(n), g = seq_len(n) %% 3L, v = (seq_len(n) * 7919L) %% 1000L)
  sql <- "SELECT k, g, v, row_number() OVER (PARTITION BY g ORDER BY v, k) AS r FROM t ORDER BY k"
  answers <- lapply(c(1L, 4L), function(threads) {
    con <- DBI::dbConnect(zugodb(), threads = threads)
    withr::defer(DBI::dbDisconnect(con))
    DBI::dbWriteTable(con, "t", value)
    DBI::dbGetQuery(con, sql)
  })
  expect_identical(answers[[2]], answers[[1]])
  # The oracle: R's own ranks.
  o <- order(value$g, value$v, value$k)
  r <- integer(n)
  r[o] <- ave(seq_len(n), value$g[o], FUN = seq_along)
  expect_identical(as.integer(answers[[1]]$r), r)
})
