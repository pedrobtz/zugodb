# Arrow IPC is refused (#40): SlothDB's Arrow reader reads a format of its
# own, so a real Arrow IPC file, here one R's arrow wrote (tools/fixtures.R),
# is refused by every route rather than parsed as that format.

test_that("a real Arrow IPC file is refused by every route", {
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  fixture <- test_path("fixtures", "arrow", "flat.arrow")
  expect_true(file.exists(fixture))
  dir <- withr::local_tempdir()
  for (ext in c("arrow", "feather", "ipc")) {
    path <- file.path(dir, paste0("flat.", ext))
    file.copy(fixture, path)
    path <- normalizePath(path, "/")
    for (sql in c("SELECT * FROM '%s'", "SELECT * FROM read_arrow('%s')",
                  "SELECT i FROM read_arrow('%s')")) {
      err <- expect_error(
        DBI::dbGetQuery(con, sprintf(sql, path)),
        class = "zug_sql_error", label = paste(ext, sql)
      )
      expect_match(
        conditionMessage(err), "Arrow IPC is not supported", fixed = TRUE
      )
    }
  }
  # The connection is still good.
  expect_identical(DBI::dbGetQuery(con, "SELECT 1 AS x")$x, 1L)
})
