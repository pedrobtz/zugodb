# #32: a Parquet scan that stopped before its last row group stopped its
# decode workers with a lost wakeup, and dbGetQuery() never returned
# (patch 0008). The queries run in a child R for a fixed time, so a hang
# fails this test, after that time and a grace period, instead of hanging
# the suite.

test_that("Parquet queries that stop early return, with their rows (#32)", {
  pkg <- find.package("zuslothdb")
  # Under load_all() the package is its source directory, which a child R
  # cannot load; R CMD check and tools/ci-run-tests.R test the installed one.
  skip_if_not(
    file.exists(file.path(pkg, "Meta", "package.rds")),
    "needs the installed package: the queries run in a child R"
  )
  # CRAN: two seconds on two threads (its limit). Elsewhere: six threads,
  # which hung the unpatched engine 4 runs in 5 within 20 seconds.
  slow <- nzchar(Sys.getenv("ZUSLOTHDB_SLOW_TESTS"))
  not_cran <- identical(Sys.getenv("NOT_CRAN"), "true")
  seconds <- if (slow) {
    120
  } else if (not_cran) {
    15
  } else {
    2
  }
  threads <- if (slow || not_cran) 6L else 2L
  fixture <- normalizePath(test_path("fixtures", "parquet-row-groups.parquet"))
  script <- withr::local_tempfile(fileext = ".R")
  writeLines(parquet_stop_child(), script)

  rscript <- file.path(R.home("bin"), "Rscript")
  out <- suppressWarnings(system2(
    rscript,
    shQuote(c(script, dirname(pkg), fixture, seconds, threads)),
    stdout = TRUE,
    stderr = TRUE,
    timeout = seconds + 120
  ))
  # 124: system2() killed the child at its timeout, a query that hung.
  expect_null(attr(out, "status"))
  last <- utils::tail(out, 1L)
  expect_match(last, "^ok [0-9]+ iterations$")
  expect_gt(as.integer(sub("^ok ([0-9]+).*", "\\1", last)), 0L)
})
