# No file is written outside tempdir() and the user's dbdir (CLAUDE.md;
# design D4). A workload that touches every route that could write (a file
# database opened, written, shut down and reopened; COPY TO, refused; an
# in-memory database) runs from a fresh working directory, and the
# directories it could write to are listed before and after: the working
# directory, dbdir's directory, and the home directory's top level.

test_that("only dbdir is written, beside nothing else", {
  work <- withr::local_tempdir()
  home <- path.expand("~")
  dbdir_dir <- withr::local_tempdir()
  path <- file.path(dbdir_dir, "h.zugodb")
  snapshot <- function() {
    list(
      work = sort(list.files(work, all.files = TRUE, recursive = TRUE, no.. = TRUE)),
      dbdir = sort(list.files(dbdir_dir, all.files = TRUE, recursive = TRUE, no.. = TRUE)),
      home = sort(list.files(home, all.files = TRUE, no.. = TRUE))
    )
  }
  before <- snapshot()
  withr::with_dir(work, {
    con <- DBI::dbConnect(zugodb(), dbdir = path)
    DBI::dbWriteTable(con, "t", data.frame(x = 1:3))
    try(DBI::dbExecute(con, "COPY t TO 'out.csv'"), silent = TRUE)
    try(DBI::dbExecute(con, "COPY t TO 'out.parquet' (FORMAT parquet)"), silent = TRUE)
    DBI::dbDisconnect(con)
    con <- DBI::dbConnect(zugodb(), dbdir = path)
    DBI::dbExecute(con, "INSERT INTO t VALUES (4)")
    DBI::dbDisconnect(con)
    mem <- DBI::dbConnect(zugodb())
    DBI::dbWriteTable(mem, "m", data.frame(s = "a"))
    DBI::dbDisconnect(mem)
  })
  after <- snapshot()
  expect_identical(after$work, before$work)
  expect_identical(after$home, before$home)
  # The database file, and no .tmp or other file beside it.
  expect_identical(after$dbdir, c(before$dbdir, "h.zugodb"))
})
