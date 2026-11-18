# SQLite, Excel and Avro files are refused (#55): SlothDB's readers for the
# first two returned wrong values against real files, and Avro's is
# untested. A real SQLite database and .xlsx (tools/fixtures.R), and a file
# with Avro's magic, are refused by every route, before any byte is parsed.

test_that("SQLite, Excel and Avro files are refused by every route", {
  con <- DBI::dbConnect(zuslothdb())
  withr::defer(DBI::dbDisconnect(con))
  dir <- withr::local_tempdir()
  fixture <- function(name) normalizePath(test_path("fixtures", "refused", name), "/")
  as_ext <- function(name, ext) {
    path <- file.path(dir, paste0(tools::file_path_sans_ext(name), ".", ext))
    file.copy(fixture(name), path, overwrite = TRUE)
    normalizePath(path, "/")
  }
  routes <- list(
    list("SELECT * FROM sqlite_scan('%s', 't')", fixture("t.sqlite"), "SQLite"),
    list("SELECT * FROM read_xlsx('%s')", fixture("t.xlsx"), "Excel"),
    list("SELECT * FROM read_avro('%s')", fixture("t.avro"), "Avro")
  )
  for (ext in c("sqlite", "sqlite3", "db")) {
    routes[[length(routes) + 1L]] <- list("SELECT * FROM '%s'", as_ext("t.sqlite", ext), "not supported")
  }
  for (ext in c("xlsx", "xls")) {
    routes[[length(routes) + 1L]] <- list("SELECT * FROM '%s'", as_ext("t.xlsx", ext), "not supported")
  }
  routes[[length(routes) + 1L]] <- list("SELECT * FROM '%s'", fixture("t.avro"), "not supported")
  for (r in routes) {
    err <- expect_error(
      DBI::dbGetQuery(con, sprintf(r[[1]], r[[2]])),
      class = "zuslothdb_sql_error", label = basename(r[[2]])
    )
    expect_match(conditionMessage(err), r[[3]], fixed = TRUE, label = basename(r[[2]]))
    expect_match(conditionMessage(err), "not supported", fixed = TRUE)
  }
  # The connection is still good.
  expect_identical(DBI::dbGetQuery(con, "SELECT 1 AS x")$x, 1L)
})
