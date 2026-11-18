# The fork guard (Q13): in a forked child the parent's connection is closed,
# because the engine's threads and file did not survive fork(); the parent's
# connection is unaffected, and the child's exit does not write the
# parent's database file.

test_that("a connection used in a forked child is a zug_closed_error", {
  skip_on_os("windows") # no fork()
  path <- withr::local_tempfile(fileext = ".sdb")
  con <- DBI::dbConnect(zugodb(), dbdir = path)
  withr::defer(DBI::dbDisconnect(con))
  DBI::dbExecute(con, "CREATE TABLE t (x INTEGER)")
  DBI::dbExecute(con, "INSERT INTO t VALUES (1), (2)")

  job <- parallel::mcparallel({
    valid <- DBI::dbIsValid(con)
    class <- tryCatch(
      {
        DBI::dbExecute(con, "INSERT INTO t VALUES (3)")
        "no error"
      },
      error = function(e) class(e)
    )
    # The child's own connections are fine: a new database in the child.
    own <- DBI::dbConnect(zugodb())
    own_ok <- DBI::dbExecute(own, "CREATE TABLE u (y INTEGER)")
    DBI::dbDisconnect(own)
    list(valid = valid, class = class, own_ok = own_ok)
  })
  got <- parallel::mccollect(job)[[1]]
  expect_false(got$valid)
  expect_true("zug_closed_error" %in% got$class)
  expect_identical(got$own_ok, 0)

  # The parent's connection still works, and the child added no row.
  expect_true(DBI::dbIsValid(con))
  expect_false(file.exists(path)) # nothing written by the child's exit
  expect_identical(DBI::dbExecute(con, "DELETE FROM t"), 2)
})
