# Transactions (design §5; roadmap Stage 5, re-scoped 2026-11-18): SlothDB
# has no undo, so dbBegin(), dbCommit() and dbRollback() are
# zug_unsupported, and SQL BEGIN, COMMIT and ROLLBACK are refused by the
# engine (libzugodb 0.3.0). Writes stay whole without a rollback:
# test-write.R checks that a refused write leaves no row.

test_that("dbBegin(), dbCommit() and dbRollback() are unsupported", {
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  expect_error(DBI::dbBegin(con), class = "zug_unsupported")
  expect_error(DBI::dbCommit(con), class = "zug_unsupported")
  expect_error(DBI::dbRollback(con), class = "zug_unsupported")
  expect_error(
    DBI::dbWithTransaction(
      con,
      DBI::dbExecute(con, "CREATE TABLE t (x INTEGER)")
    ),
    class = "zug_unsupported"
  )
  # dbWithTransaction() ran nothing.
  expect_false(DBI::dbExistsTable(con, "t"))
})

test_that("SQL BEGIN, COMMIT and ROLLBACK are refused, and each write is kept", {
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  DBI::dbExecute(con, "CREATE TABLE t (x INTEGER)")
  DBI::dbExecute(con, "INSERT INTO t VALUES (1)")
  for (sql in c("BEGIN", "BEGIN TRANSACTION", "COMMIT", "ROLLBACK")) {
    err <- expect_error(DBI::dbExecute(con, sql), class = "zug_sql_error", label = sql)
    expect_match(conditionMessage(err), "Transactions are not supported", fixed = TRUE)
  }
  DBI::dbExecute(con, "INSERT INTO t VALUES (2)")
  expect_identical(DBI::dbGetQuery(con, "SELECT x FROM t ORDER BY x")$x, 1:2)
})
