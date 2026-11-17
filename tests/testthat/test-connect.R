# dbConnect()'s arguments (roadmap Stage 2; design §5, D5).

test_that("threads: 2 by default, 1 and 2 accepted and reported", {
  con <- DBI::dbConnect(zuslothdb())
  expect_identical(zuslothdb_info()$threads, 2L)
  DBI::dbDisconnect(con)
  for (n in c(1L, 2L)) {
    con <- DBI::dbConnect(zuslothdb(), threads = n)
    expect_identical(zuslothdb_info()$threads, n)
    DBI::dbDisconnect(con)
  }
  # A double holding a whole number is taken as that number.
  con <- DBI::dbConnect(zuslothdb(), threads = 1)
  expect_identical(zuslothdb_info()$threads, 1L)
  DBI::dbDisconnect(con)
  DBI::dbDisconnect(DBI::dbConnect(zuslothdb()))
  expect_identical(zuslothdb_info()$threads, 2L)
})

test_that("threads straddles its range: 0 and integer.max + 1 refused", {
  big <- .Machine$integer.max
  for (bad in list(0, -1, big + 1, Inf, NA, NA_integer_, 1.5, "2", c(1, 2),
                   integer())) {
    expect_error(
      DBI::dbConnect(zuslothdb(), threads = bad),
      class = "zuslothdb_argument_error"
    )
  }
  # The cap, not a thread count: the pool never starts more threads than
  # the hardware has.
  con <- DBI::dbConnect(zuslothdb(), threads = big)
  expect_identical(zuslothdb_info()$threads, big)
  expect_identical(DBI::dbExecute(con, "CREATE TABLE t (x INTEGER)"), 0)
  DBI::dbDisconnect(con)
  DBI::dbDisconnect(DBI::dbConnect(zuslothdb(), threads = 2L))
})

test_that("read_only = TRUE refuses writes and leaves the file's bytes alone", {
  path <- withr::local_tempfile(fileext = ".sdb")
  con <- DBI::dbConnect(zuslothdb(), dbdir = path)
  DBI::dbExecute(con, "CREATE TABLE t (x INTEGER)")
  DBI::dbExecute(con, "INSERT INTO t VALUES (1)")
  DBI::dbDisconnect(con)
  before <- tools::md5sum(path)

  ro <- DBI::dbConnect(zuslothdb(), dbdir = path, read_only = TRUE)
  for (sql in c(
    "INSERT INTO t VALUES (2)",
    "DELETE FROM t",
    "UPDATE t SET x = 3",
    "CREATE TABLE u (y INTEGER)",
    "DROP TABLE t",
    # A batch is refused whole: the SELECT does not run first.
    "SELECT 1; INSERT INTO t VALUES (4)"
  )) {
    expect_error(DBI::dbExecute(ro, sql), class = "zuslothdb_sql_error")
  }
  expect_true(DBI::dbIsValid(ro))
  expect_true(DBI::dbGetInfo(ro)$read_only)
  DBI::dbDisconnect(ro)
  expect_identical(tools::md5sum(path), before)

  # And the data is as it was: one row.
  rw <- DBI::dbConnect(zuslothdb(), dbdir = path)
  withr::defer(DBI::dbDisconnect(rw))
  expect_identical(DBI::dbExecute(rw, "DELETE FROM t"), 1)
})

test_that("a database that cannot be opened as asked is refused up front", {
  missing_file <- file.path(withr::local_tempdir(), "none.sdb")
  expect_error(
    zuslothdb(missing_file, read_only = TRUE),
    class = "zuslothdb_argument_error"
  )
  expect_error(
    zuslothdb(":memory:", read_only = TRUE),
    class = "zuslothdb_argument_error"
  )
  expect_error(
    zuslothdb(file.path(withr::local_tempdir(), "no", "such", "dir.sdb")),
    class = "zuslothdb_argument_error"
  )
  expect_error(
    zuslothdb(withr::local_tempdir()),
    class = "zuslothdb_argument_error"
  )
  for (bad in list(NA_character_, character(), c("a", "b"), 1)) {
    expect_error(zuslothdb(bad), class = "zuslothdb_argument_error")
  }
  for (bad in list(NA, "yes", c(TRUE, FALSE), logical())) {
    expect_error(
      zuslothdb(read_only = bad),
      class = "zuslothdb_argument_error"
    )
  }
})

test_that("dbConnect(dbdir = ) gives a database of its own", {
  drv <- zuslothdb()
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
    class = "zuslothdb_sql_error"
  )
})

test_that("an unknown argument to dbConnect() is an error", {
  expect_error(
    DBI::dbConnect(zuslothdb(), memory_limit = "1GB"),
    class = "zuslothdb_argument_error"
  )
})
