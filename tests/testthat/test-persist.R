# Persistence (design §8, Q7; roadmap Stage 6). A database file is read
# whole when its database opens and written whole when it shuts down. Its
# format is version 1001 with a CRC-32 (libzugodb 0.2.0): another version
# is an error naming both, before any table is read.

test_that("a database written here reads back identical in a fresh R process", {
  dir <- withr::local_tempdir()
  path <- normalizePath(file.path(dir, "p.zugodb"), "/", mustWork = FALSE)
  value <- persist_value()
  con <- DBI::dbConnect(zugodb(), dbdir = path)
  DBI::dbWriteTable(con, "t", value)
  DBI::dbExecute(con, "CREATE TABLE u AS SELECT CAST(i AS BIGINT) * 2 AS j FROM t WHERE i > 0")
  DBI::dbDisconnect(con, shutdown = TRUE)
  expect_true(file.exists(path))

  # The child opens the file read-only, reads both tables and saves them.
  out <- normalizePath(file.path(dir, "out.rds"), "/", mustWork = FALSE)
  script <- file.path(dir, "child.R")
  writeLines(c(
    "args <- commandArgs(trailingOnly = TRUE)",
    "con <- DBI::dbConnect(zugodb::zugodb(), dbdir = args[[1]], read_only = TRUE)",
    "x <- list(t = DBI::dbReadTable(con, 't'),",
    "          u = DBI::dbGetQuery(con, 'SELECT j FROM u ORDER BY j'),",
    "          tables = sort(DBI::dbListTables(con)))",
    "DBI::dbDisconnect(con)",
    "saveRDS(x, args[[2]])"
  ), script)
  res <- run_child_r(script, c(path, out), timeout = 120)
  expect_null(attr(res, "status"))
  got <- readRDS(out)
  expect_identical(got$tables, c("t", "u"))
  expect_identical(got$t, value)
  expect_identical(got$u$j, c(2, 4294967294))
})

test_that("a change made after reopening is in the file at the next open", {
  path <- withr::local_tempfile(fileext = ".zugodb")
  con <- DBI::dbConnect(zugodb(), dbdir = path)
  DBI::dbWriteTable(con, "t", data.frame(x = 1:3))
  DBI::dbDisconnect(con)
  con <- DBI::dbConnect(zugodb(), dbdir = path)
  DBI::dbExecute(con, "DELETE FROM t WHERE x = 2")
  DBI::dbWriteTable(con, "t", data.frame(x = 4L), append = TRUE)
  DBI::dbExecute(con, "CREATE TABLE v (s VARCHAR)")
  DBI::dbDisconnect(con)
  con <- DBI::dbConnect(zugodb(), dbdir = path)
  withr::defer(DBI::dbDisconnect(con))
  expect_identical(DBI::dbGetQuery(con, "SELECT x FROM t ORDER BY x")$x, c(1L, 3L, 4L))
  expect_identical(sort(DBI::dbListTables(con)), c("t", "v"))
})

test_that("more rows than a row group round-trip through the file", {
  skip_if_heavy()
  path <- withr::local_tempfile(fileext = ".zugodb")
  n <- 300000L
  value <- data.frame(
    k = seq_len(n),
    v = ifelse(seq_len(n) %% 11L == 0L, NA, seq_len(n) / 4),
    s = ifelse(seq_len(n) %% 13L == 0L, NA, paste0("s", seq_len(n)))
  )
  con <- DBI::dbConnect(zugodb(), dbdir = path)
  DBI::dbWriteTable(con, "big", value)
  DBI::dbDisconnect(con)
  con <- DBI::dbConnect(zugodb(), dbdir = path, read_only = TRUE)
  withr::defer(DBI::dbDisconnect(con))
  expect_identical(DBI::dbGetQuery(con, "SELECT * FROM big ORDER BY k"), value)
})

test_that("the golden database file reads back as its recorded rows", {
  path <- golden_copy(withr::local_tempdir())
  rows <- golden_rows()
  con <- DBI::dbConnect(zugodb(), dbdir = path, read_only = TRUE, bigint = "character")
  withr::defer(DBI::dbDisconnect(con))
  expect_setequal(DBI::dbListTables(con), c(names(rows), "many"))
  for (t in names(rows)) {
    got <- DBI::dbGetQuery(con, sprintf("SELECT * FROM \"%s\" ORDER BY 1", t))
    expect_identical(got, rows[[t]], label = t)
  }
  expect_identical(DBI::dbGetQuery(con, "SELECT * FROM many ORDER BY k"), golden_many())
})

test_that("a file of another format version is an error naming both versions", {
  dir <- withr::local_tempdir()
  for (version in c(1L, 1000L, 1002L, 2147483647L)) {
    path <- golden_copy(dir)
    bytes <- read_bytes(path)
    bytes[5:8] <- writeBin(version, raw(), size = 4, endian = "little")
    writeBin(bytes, path)
    err <- expect_error(
      DBI::dbConnect(zugodb(), dbdir = path),
      class = "zug_sql_error",
      label = version
    )
    expect_match(conditionMessage(err), paste("format version", version), fixed = TRUE)
    expect_match(conditionMessage(err), "version 1001 only", fixed = TRUE)
    # Refused before anything is written: the file is as it was.
    expect_identical(read_bytes(path), bytes, label = version)
    unlink(path)
  }
  # A database of another driver opens.
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  expect_identical(DBI::dbGetQuery(con, "SELECT 1 AS x")$x, 1L)
})
