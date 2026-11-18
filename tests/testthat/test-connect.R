# dbConnect()'s arguments (roadmap Stage 2; design §5, D5).

test_that("threads: 2 by default, 1 and 2 accepted and reported", {
  con <- DBI::dbConnect(zugodb())
  expect_identical(zug_info()$threads, 2L)
  DBI::dbDisconnect(con)
  for (n in c(1L, 2L)) {
    con <- DBI::dbConnect(zugodb(), threads = n)
    expect_identical(zug_info()$threads, n)
    DBI::dbDisconnect(con)
  }
  # A double holding a whole number is taken as that number.
  con <- DBI::dbConnect(zugodb(), threads = 1)
  expect_identical(zug_info()$threads, 1L)
  DBI::dbDisconnect(con)
  DBI::dbDisconnect(DBI::dbConnect(zugodb()))
  expect_identical(zug_info()$threads, 2L)
})

test_that("threads straddles its range: 0 and integer.max + 1 refused", {
  big <- .Machine$integer.max
  for (bad in list(0, -1, big + 1, Inf, NA, NA_integer_, 1.5, "2", c(1, 2),
                   integer())) {
    expect_error(
      DBI::dbConnect(zugodb(), threads = bad),
      class = "zug_argument_error"
    )
  }
  # The cap, not a thread count: the pool never starts more threads than
  # the hardware has.
  con <- DBI::dbConnect(zugodb(), threads = big)
  expect_identical(zug_info()$threads, big)
  expect_identical(DBI::dbExecute(con, "CREATE TABLE t (x INTEGER)"), 0)
  DBI::dbDisconnect(con)
  DBI::dbDisconnect(DBI::dbConnect(zugodb(), threads = 2L))
})

test_that("read_only = TRUE refuses writes and leaves the file's bytes alone", {
  path <- withr::local_tempfile(fileext = ".sdb")
  con <- DBI::dbConnect(zugodb(), dbdir = path)
  DBI::dbExecute(con, "CREATE TABLE t (x INTEGER)")
  DBI::dbExecute(con, "INSERT INTO t VALUES (1)")
  DBI::dbDisconnect(con)
  before <- tools::md5sum(path)

  ro <- DBI::dbConnect(zugodb(), dbdir = path, read_only = TRUE)
  for (sql in c(
    "INSERT INTO t VALUES (2)",
    "DELETE FROM t",
    "UPDATE t SET x = 3",
    "CREATE TABLE u (y INTEGER)",
    "DROP TABLE t",
    # A batch is refused whole: the SELECT does not run first.
    "SELECT 1; INSERT INTO t VALUES (4)"
  )) {
    expect_error(DBI::dbExecute(ro, sql), class = "zug_sql_error")
  }
  expect_true(DBI::dbIsValid(ro))
  expect_true(DBI::dbGetInfo(ro)$read_only)
  DBI::dbDisconnect(ro)
  expect_identical(tools::md5sum(path), before)

  # And the data is as it was: one row.
  rw <- DBI::dbConnect(zugodb(), dbdir = path)
  withr::defer(DBI::dbDisconnect(rw))
  expect_identical(DBI::dbExecute(rw, "DELETE FROM t"), 1)
})

test_that("a database that cannot be opened as asked is refused up front", {
  missing_file <- file.path(withr::local_tempdir(), "none.sdb")
  expect_error(
    zugodb(missing_file, read_only = TRUE),
    class = "zug_argument_error"
  )
  expect_error(
    zugodb(":memory:", read_only = TRUE),
    class = "zug_argument_error"
  )
  expect_error(
    zugodb(file.path(withr::local_tempdir(), "no", "such", "dir.sdb")),
    class = "zug_argument_error"
  )
  expect_error(
    zugodb(withr::local_tempdir()),
    class = "zug_argument_error"
  )
  for (bad in list(NA_character_, character(), c("a", "b"), 1)) {
    expect_error(zugodb(bad), class = "zug_argument_error")
  }
  for (bad in list(NA, "yes", c(TRUE, FALSE), logical())) {
    expect_error(
      zugodb(read_only = bad),
      class = "zug_argument_error"
    )
  }
})

test_that("dbConnect(dbdir = ) gives a database of its own", {
  drv <- zugodb()
  a <- DBI::dbConnect(drv)
  withr::defer(DBI::dbDisconnect(a))
  path <- withr::local_tempfile(fileext = ".sdb")
  b <- DBI::dbConnect(drv, dbdir = path)
  withr::defer(DBI::dbDisconnect(b))
  # The directory resolved (so /var and /private/var are one), the file
  # name kept: the file does not exist yet.
  expect_identical(
    DBI::dbGetInfo(b)$dbname,
    file.path(normalizePath(dirname(path), winslash = "/"), basename(path))
  )
  DBI::dbExecute(a, "CREATE TABLE t (x INTEGER)")
  # b's database does not have a's table.
  expect_error(
    DBI::dbExecute(b, "INSERT INTO t VALUES (1)"),
    class = "zug_sql_error"
  )
})

test_that("an unknown argument to dbConnect() is an error", {
  expect_error(
    DBI::dbConnect(zugodb(), memory_limit = "1GB"),
    class = "zug_argument_error"
  )
})

test_that("dbConnect() checks dbdir and read_only as zugodb() does", {
  drv <- zugodb()
  for (bad in list(NA_character_, character(), c("a", "b"), 1)) {
    expect_error(
      DBI::dbConnect(drv, dbdir = bad),
      class = "zug_argument_error"
    )
  }
  for (bad in list(NA, "yes", c(TRUE, FALSE), logical())) {
    expect_error(
      DBI::dbConnect(drv, read_only = bad),
      class = "zug_argument_error"
    )
  }
})

test_that("dbDisconnect() checks shutdown, and leaves the connection open", {
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  for (bad in list(NA, "yes", c(TRUE, FALSE), logical(), 1)) {
    expect_error(
      DBI::dbDisconnect(con, shutdown = bad),
      class = "zug_argument_error"
    )
  }
  expect_true(DBI::dbIsValid(con))
})

test_that("a database in a directory with a non-ASCII name round-trips", {
  dir <- file.path(withr::local_tempdir(), "données 数据")
  dir.create(dir)
  path <- file.path(dir, "bäse.sdb")
  con <- DBI::dbConnect(zugodb(), dbdir = path)
  DBI::dbExecute(con, "CREATE TABLE t (x INTEGER)")
  DBI::dbExecute(con, "INSERT INTO t VALUES (1), (2), (3)")
  DBI::dbDisconnect(con)
  # Written where it was asked to be, under that name.
  expect_true(file.exists(path))
  expect_identical(list.files(dir), "bäse.sdb")
  con <- DBI::dbConnect(zugodb(), dbdir = path)
  withr::defer(DBI::dbDisconnect(con))
  expect_identical(DBI::dbExecute(con, "DELETE FROM t"), 3)
})

test_that("the glue checks its own arguments", {
  # R checks them first; these guard the .Call entry points themselves.
  expect_identical(.Call(zug_db_open, NA_character_, FALSE)$kind, "argument")
  expect_identical(.Call(zug_db_open, "", NA)$kind, "argument")
  expect_identical(.Call(zug_set_threads, 0L)$kind, "argument")
  expect_identical(.Call(zug_set_threads, 2)$kind, "argument") # not integer
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  expect_identical(.Call(zug_execute, con@state$ptr, NA_character_)$kind, "argument")
  expect_identical(.Call(zug_execute, con@state$ptr, 1)$kind, "argument")
  # Not an external pointer at all: closed, not a crash.
  expect_identical(.Call(zug_execute, NULL, "SELECT 1")$kind, "closed")
  expect_identical(.Call(zug_connect, NULL)$kind, "closed")
  expect_false(.Call(zug_conn_is_open, NULL))
})
