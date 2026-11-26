# Databases and connections used out of order (roadmap Stage 2; design
# §4.3, Q9). Each sequence ends in its documented state: a
# zug_closed_error, a warning, or a working connection. The oracle for
# "the data is still there" is the row count DELETE reports, since results
# arrive only in Stage 3.

test_that("disconnecting twice warns and returns FALSE", {
  con <- DBI::dbConnect(zugodb())
  expect_true(DBI::dbDisconnect(con))
  expect_warning(res <- DBI::dbDisconnect(con), "already closed")
  expect_false(res)
})

test_that("a disconnected connection is a zug_closed_error", {
  con <- DBI::dbConnect(zugodb())
  DBI::dbDisconnect(con)
  expect_false(DBI::dbIsValid(con))
  expect_error(
    DBI::dbExecute(con, "CREATE TABLE t (x INTEGER)"),
    class = "zug_closed_error"
  )
})

test_that("two connections from one driver share its database", {
  drv <- zugodb()
  a <- DBI::dbConnect(drv)
  b <- DBI::dbConnect(drv)
  withr::defer({
    DBI::dbDisconnect(a)
    DBI::dbDisconnect(b)
  })
  DBI::dbExecute(a, "CREATE TABLE t (x INTEGER)")
  expect_identical(DBI::dbExecute(b, "INSERT INTO t VALUES (1), (2)"), 2)
  expect_identical(DBI::dbExecute(a, "DELETE FROM t"), 2)
})

test_that("disconnecting one connection leaves the others open", {
  drv <- zugodb()
  a <- DBI::dbConnect(drv)
  b <- DBI::dbConnect(drv)
  withr::defer(DBI::dbDisconnect(b))
  DBI::dbExecute(a, "CREATE TABLE t (x INTEGER)")
  DBI::dbDisconnect(a, shutdown = TRUE)
  expect_true(DBI::dbIsValid(b))
  expect_identical(DBI::dbExecute(b, "INSERT INTO t VALUES (1)"), 1)
})

test_that("after shutdown the old connection is closed and a new one starts afresh", {
  drv <- zugodb()
  a <- DBI::dbConnect(drv)
  DBI::dbExecute(a, "CREATE TABLE t (x INTEGER)")
  DBI::dbDisconnect(a) # the last connection: the database closes
  expect_error(
    DBI::dbExecute(a, "INSERT INTO t VALUES (1)"),
    class = "zug_closed_error"
  )
  b <- DBI::dbConnect(drv)
  withr::defer(DBI::dbDisconnect(b))
  # A new in-memory database: the old table is gone.
  expect_error(
    DBI::dbExecute(b, "INSERT INTO t VALUES (1)"),
    class = "zug_sql_error"
  )
})

test_that("shutdown = FALSE keeps the database open for the next connection", {
  drv <- zugodb()
  a <- DBI::dbConnect(drv)
  DBI::dbExecute(a, "CREATE TABLE t (x INTEGER)")
  DBI::dbExecute(a, "INSERT INTO t VALUES (1), (2), (3)")
  DBI::dbDisconnect(a, shutdown = FALSE)
  b <- DBI::dbConnect(drv)
  withr::defer(DBI::dbDisconnect(b))
  expect_identical(DBI::dbExecute(b, "DELETE FROM t"), 3)
})

test_that("a file-backed database is written when its last connection closes", {
  path <- withr::local_tempfile(fileext = ".sdb")
  a <- DBI::dbConnect(zugodb(), dbdir = path)
  DBI::dbExecute(a, "CREATE TABLE t (x INTEGER)")
  DBI::dbExecute(a, "INSERT INTO t VALUES (1), (2)")
  expect_false(file.exists(path))
  DBI::dbDisconnect(a)
  expect_true(file.exists(path))
  b <- DBI::dbConnect(zugodb(), dbdir = path)
  withr::defer(DBI::dbDisconnect(b))
  expect_identical(DBI::dbExecute(b, "DELETE FROM t"), 2)
})

test_that("the driver collected while a connection lives leaves it working", {
  local({
    drv <- zugodb()
    con <<- DBI::dbConnect(drv)
  })
  withr::defer(DBI::dbDisconnect(con))
  gc()
  expect_true(DBI::dbIsValid(con))
  expect_identical(DBI::dbExecute(con, "CREATE TABLE t (x INTEGER)"), 0)
})

# R runs finalizers through R_ToplevelExec, so a warning a finalizer raises
# reaches no handler of gc()'s caller: the collection is tested by its
# effect (the database closed and written), the warning by calling the
# finalizer itself.

test_that("a connection collected without dbDisconnect() is closed and written", {
  path <- withr::local_tempfile(fileext = ".sdb")
  local({
    con <- DBI::dbConnect(zugodb(), dbdir = path)
    DBI::dbExecute(con, "CREATE TABLE t (x INTEGER)")
    DBI::dbExecute(con, "INSERT INTO t VALUES (7)")
  })
  withr::with_options(list(warn = -1), gc())
  # It was the last connection, so the database closed and was written.
  expect_true(file.exists(path))
  b <- DBI::dbConnect(zugodb(), dbdir = path)
  withr::defer(DBI::dbDisconnect(b))
  expect_identical(DBI::dbExecute(b, "DELETE FROM t"), 1)
})

test_that("a connection's finalizer warns, and only while it is open", {
  con <- DBI::dbConnect(zugodb())
  expect_warning(connection_finalizer(con@state), "garbage-collected")
  expect_false(DBI::dbIsValid(con))
  expect_no_warning(connection_finalizer(con@state))
})

test_that("a database kept open and then collected is written", {
  path <- withr::local_tempfile(fileext = ".sdb")
  local({
    drv <- zugodb(dbdir = path)
    con <- DBI::dbConnect(drv)
    DBI::dbExecute(con, "CREATE TABLE t (x INTEGER)")
    DBI::dbExecute(con, "INSERT INTO t VALUES (1), (2), (3), (4)")
    DBI::dbDisconnect(con, shutdown = FALSE)
  })
  withr::with_options(list(warn = -1), gc())
  expect_true(file.exists(path))
  b <- DBI::dbConnect(zugodb(), dbdir = path)
  withr::defer(DBI::dbDisconnect(b))
  expect_identical(DBI::dbExecute(b, "DELETE FROM t"), 4)
})

test_that("a driver's finalizer warns only for a database left open", {
  drv <- zugodb()
  con <- DBI::dbConnect(drv)
  DBI::dbDisconnect(con, shutdown = FALSE)
  expect_warning(driver_finalizer(drv@state), "garbage-collected")
  expect_false(.Call(zug_db_is_open, drv@state$db))
  expect_no_warning(driver_finalizer(drv@state))
  expect_no_warning(driver_finalizer(zugodb()@state)) # never opened
})

test_that("show() reports the database and whether it is open", {
  drv <- zugodb()
  con <- DBI::dbConnect(drv)
  expect_output(show(drv), "<zugodb_driver> :memory:")
  expect_output(show(con), "<zugodb_connection> :memory:$")
  DBI::dbDisconnect(con)
  expect_output(show(con), "DISCONNECTED")
})

test_that("dbGetInfo() names the database and the engine commit", {
  drv <- zugodb()
  con <- DBI::dbConnect(drv)
  withr::defer(DBI::dbDisconnect(con))
  info <- DBI::dbGetInfo(con)
  expect_identical(info$dbname, ":memory:")
  expect_identical(info$db.version, zug_info()$commit)
  expect_false(info$read_only)
  expect_identical(
    DBI::dbGetInfo(drv)$driver.version,
    utils::packageVersion("zugodb")
  )
})

# Results (roadmap Stage 3).

test_that("fetching after dbClearResult() is a zug_closed_error", {
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  res <- DBI::dbSendQuery(con, "SELECT 1 AS a")
  expect_true(DBI::dbClearResult(res))
  expect_false(DBI::dbIsValid(res))
  expect_error(DBI::dbFetch(res), class = "zug_closed_error")
  expect_error(DBI::dbHasCompleted(res), class = "zug_closed_error")
})

test_that("clearing twice warns and returns FALSE", {
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  res <- DBI::dbSendQuery(con, "SELECT 1 AS a")
  DBI::dbClearResult(res)
  expect_warning(out <- DBI::dbClearResult(res), "already cleared")
  expect_false(out)
})

test_that("fetching after dbDisconnect() is a zug_closed_error", {
  con <- DBI::dbConnect(zugodb())
  res <- DBI::dbSendQuery(con, "SELECT 1 AS a")
  DBI::dbDisconnect(con)
  expect_false(DBI::dbIsValid(res))
  expect_error(DBI::dbFetch(res), class = "zug_closed_error")
})

test_that("a second query while one is open clears the first, with a warning", {
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  first <- DBI::dbSendQuery(con, "SELECT 1 AS a")
  expect_warning(second <- DBI::dbSendQuery(con, "SELECT 2 AS b"), "Clearing")
  expect_false(DBI::dbIsValid(first))
  expect_error(DBI::dbFetch(first), class = "zug_closed_error")
  expect_identical(DBI::dbFetch(second)$b, 2L)
  DBI::dbClearResult(second)
  # A cleared result does not count as open: no warning for the next.
  expect_no_warning(third <- DBI::dbSendQuery(con, "SELECT 3 AS c"))
  DBI::dbClearResult(third)
})

test_that("show() reports a result and whether it is cleared", {
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  res <- DBI::dbSendQuery(con, "SELECT 1 AS a")
  expect_output(show(res), "<zugodb_result> SELECT 1 AS a$")
  DBI::dbClearResult(res)
  expect_output(show(res), "\\(cleared\\)")
})

test_that("transaction statements in any order are engine errors", {
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  for (sql in c("COMMIT", "ROLLBACK", "BEGIN", "BEGIN", "COMMIT")) {
    expect_error(DBI::dbExecute(con, sql), class = "zug_sql_error", label = sql)
  }
  # The connection is not left inside anything.
  expect_identical(DBI::dbGetQuery(con, "SELECT 1 AS x")$x, 1L)
})

test_that("a refused BEGIN leaves later writes in the file", {
  path <- withr::local_tempfile(fileext = ".db")
  con <- DBI::dbConnect(zugodb(), dbdir = path)
  DBI::dbExecute(con, "CREATE TABLE t (x INTEGER)")
  expect_error(DBI::dbExecute(con, "BEGIN"), class = "zug_sql_error")
  DBI::dbWriteTable(con, "t", data.frame(x = 1:3), append = TRUE)
  DBI::dbDisconnect(con)
  con2 <- DBI::dbConnect(zugodb(), dbdir = path)
  withr::defer(DBI::dbDisconnect(con2))
  expect_identical(DBI::dbReadTable(con2, "t")$x, 1:3)
})

test_that("table functions on a closed connection are zug_closed_error", {
  con <- DBI::dbConnect(zugodb())
  DBI::dbWriteTable(con, "t", data.frame(x = 1L))
  DBI::dbDisconnect(con)
  df <- data.frame(x = 2L)
  expect_error(DBI::dbWriteTable(con, "t", df, append = TRUE), class = "zug_closed_error")
  expect_error(DBI::dbAppendTable(con, "t", df), class = "zug_closed_error")
  expect_error(DBI::dbListTables(con), class = "zug_closed_error")
  expect_error(DBI::dbListFields(con, "t"), class = "zug_closed_error")
  expect_error(DBI::dbExistsTable(con, "t"), class = "zug_closed_error")
  expect_error(DBI::dbRemoveTable(con, "t"), class = "zug_closed_error")
})

# Arrow C streams (roadmap Stage 7): a stream is read once and released by
# the load that reads it (src/zug_stream.cpp).

test_that("a stream released by its producer is a zug_closed_error", {
  skip_if_not_installed("zuarrow")
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  stream <- zuarrow::arrow_stream(data.frame(x = 1:3))
  zuarrow::arrow_release(stream)
  expect_error(
    DBI::dbWriteTableArrow(con, "t", stream),
    class = "zug_closed_error"
  )
  expect_error(
    DBI::dbAppendTableArrow(con, "t", stream),
    class = "zug_argument_error"
  )
  DBI::dbExecute(con, "CREATE TABLE t (x INTEGER)")
  expect_error(
    DBI::dbAppendTableArrow(con, "t", stream),
    class = "zug_closed_error"
  )
  expect_identical(DBI::dbGetQuery(con, "SELECT count(*) AS n FROM t")$n, 0)
})

test_that("a stream read twice is a zug_closed_error the second time", {
  skip_if_not_installed("zuarrow")
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  stream <- zuarrow::arrow_stream(data.frame(x = 1:3))
  DBI::dbWriteTableArrow(con, "t", stream)
  # The load took the stream: the producer's object is left released.
  expect_error(zuarrow::arrow_next(stream), class = "zuarrow_handle_error")
  expect_error(
    DBI::dbWriteTableArrow(con, "t", stream, append = TRUE),
    class = "zug_closed_error"
  )
  expect_error(
    DBI::dbWriteTableArrow(con, "u", stream),
    class = "zug_closed_error"
  )
  expect_false(DBI::dbExistsTable(con, "u"))
  expect_identical(DBI::dbGetQuery(con, "SELECT count(*) AS n FROM t")$n, 3)
})

test_that("a batch whose layout differs from the stream's schema is a zug_stream_error", {
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  before <- zug_scripted_releases()
  # Another number of columns at the second batch.
  stream <- zug_scripted_stream(4, 10, fail_at = 2, mismatch = TRUE)
  expect_error(
    DBI::dbWriteTableArrow(con, "t", stream),
    class = "zug_stream_error"
  )
  expect_false(DBI::dbExistsTable(con, "t"))
  expect_identical(zug_scripted_releases() - before, 1L)
  # An int32 column given a string's three buffers.
  wrong <- bytes_column("u", list(charToRaw("a"), charToRaw("b")))
  wrong$format <- "i"
  err <- expect_error(
    DBI::dbWriteTableArrow(con, "t", zug_raw_stream(list(x = wrong), length = 2)),
    class = "zug_stream_error"
  )
  expect_match(conditionMessage(err), "Column `x`", fixed = TRUE)
  expect_false(DBI::dbExistsTable(con, "t"))
  # A column shorter than its batch.
  short <- fixed_column("i", int32_bytes(1:2), 2)
  expect_error(
    DBI::dbWriteTableArrow(con, "t", zug_raw_stream(list(x = short), length = 3)),
    class = "zug_stream_error"
  )
  expect_false(DBI::dbExistsTable(con, "t"))
})

test_that("a load on a closed connection leaves the stream to its producer", {
  skip_if_not_installed("zuarrow")
  con <- DBI::dbConnect(zugodb())
  DBI::dbDisconnect(con)
  stream <- zuarrow::arrow_stream(data.frame(x = 1:3))
  expect_error(
    DBI::dbWriteTableArrow(con, "t", stream),
    class = "zug_closed_error"
  )
  expect_error(
    DBI::dbAppendTableArrow(con, "t", stream),
    class = "zug_closed_error"
  )
  expect_error(
    DBI::dbCreateTableArrow(con, "t", stream),
    class = "zug_closed_error"
  )
  expect_identical(as.data.frame(stream)$x, 1:3)
})

test_that("an error after the stream is taken releases it once", {
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  DBI::dbExecute(con, "CREATE TABLE t (y INTEGER)")
  before <- zug_scripted_releases()
  # The stream's columns are not the table's: refused after the take.
  expect_error(
    DBI::dbAppendTableArrow(con, "t", zug_scripted_stream(2, 5)),
    class = "zug_argument_error"
  )
  expect_identical(zug_scripted_releases() - before, 1L)
  # A stream never read is released by the finalizer, once.
  before <- zug_scripted_releases()
  local(zug_scripted_stream(2, 5))
  gc()
  expect_identical(zug_scripted_releases() - before, 1L)
})
