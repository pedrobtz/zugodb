# Transactions (design §5; roadmap Stage 5, re-scoped 2026-11-18): SlothDB
# has no undo, so dbBegin(), dbCommit() and dbRollback() are
# zug_unsupported, and SQL ROLLBACK, by the fork's change, is an error
# saying that nothing was undone. Writes stay whole without a rollback:
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

test_that("SQL ROLLBACK says that nothing was undone, and the change is kept", {
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  DBI::dbExecute(con, "CREATE TABLE t (x INTEGER)")
  DBI::dbExecute(con, "INSERT INTO t VALUES (1)")
  DBI::dbExecute(con, "BEGIN")
  DBI::dbExecute(con, "INSERT INTO t VALUES (2)")
  err <- expect_error(
    DBI::dbExecute(con, "ROLLBACK"),
    class = "zug_sql_error"
  )
  expect_match(conditionMessage(err), "does not undo", fixed = TRUE)
  expect_identical(DBI::dbGetQuery(con, "SELECT x FROM t ORDER BY x")$x, 1:2)
  # The transaction was closed: a new one begins, a second ROLLBACK is one
  # without a transaction.
  expect_identical(DBI::dbExecute(con, "BEGIN"), 0)
  expect_identical(DBI::dbExecute(con, "COMMIT"), 0)
  expect_error(DBI::dbExecute(con, "ROLLBACK"), class = "zug_sql_error")
})
