# The unwind guard (design §4.3): an error during a call, from the engine or
# from R, leaves the connection valid, and no engine object is leaked. The
# leak half is valgrind's (native-checks.yaml): these loops make each
# failing path run many times, so a leak per call would be reported.

test_that("engine errors leave the connection valid and working", {
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  DBI::dbExecute(con, "CREATE TABLE t (x INTEGER)")
  for (i in seq_len(50)) {
    expect_error(DBI::dbExecute(con, "SELEC 1"), class = "zug_sql_error")
    expect_error(
      DBI::dbExecute(con, "INSERT INTO missing VALUES (1)"),
      class = "zug_sql_error"
    )
  }
  expect_true(DBI::dbIsValid(con))
  expect_identical(DBI::dbExecute(con, "INSERT INTO t VALUES (1), (2)"), 2)
})

test_that("an R error between engine calls leaves the connection valid", {
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  DBI::dbExecute(con, "CREATE TABLE t (x INTEGER)")
  for (i in seq_len(50)) {
    # An R-level failure in the middle of the call: the argument check, and
    # an error raised by a handler while the connection is in use.
    expect_error(DBI::dbExecute(con, NA_character_), class = "zug_error")
    expect_error(
      withCallingHandlers(
        DBI::dbExecute(con, "SELEC 1"),
        zug_sql_error = function(e) stop("an R error in a handler")
      ),
      "an R error in a handler"
    )
  }
  expect_true(DBI::dbIsValid(con))
  expect_identical(DBI::dbExecute(con, "INSERT INTO t VALUES (1)"), 1)
})

test_that("connections opened and closed in a loop release their engines", {
  drv <- zugodb()
  for (i in seq_len(50)) {
    con <- DBI::dbConnect(drv)
    DBI::dbExecute(con, "CREATE TABLE t (x INTEGER)")
    DBI::dbDisconnect(con)
  }
  expect_false(DBI::dbIsValid(con))
})
