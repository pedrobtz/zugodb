# #32: a Parquet scan that stopped before its last row group stopped its
# decode workers with a lost wakeup, and dbGetQuery() never returned
# (patch 0008). #34: on Windows, those workers corrupted the heap as they
# exited, and R died with 0xC0000409, shown as status 1033 (patch 0009).
# The queries run in a child R for a fixed time, so a hang or a crash
# fails this test, after that time and a grace period, instead of taking
# the suite with it.

test_that("Parquet queries on decode workers return their rows (#32, #34)", {
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
  trace <- withr::local_tempfile(fileext = ".txt")

  out <- run_child_r(
    script,
    c(dirname(pkg), fixture, seconds, threads, trace),
    timeout = seconds + 120
  )
  steps <- if (file.exists(trace)) readLines(trace) else "(no trace file)"
  said <- paste(
    c("child R printed:", out, "its steps:", steps),
    collapse = "\n"
  )
  # 124: system2() killed the child at its timeout, a query that hung;
  # 1033 on Windows: the child died (#34).
  expect_null(attr(out, "status"), info = said)
  last <- utils::tail(out, 1L)
  expect_match(last, "^ok [0-9]+ iterations$", info = said)
  expect_gt(as.integer(sub("^ok ([0-9]+).*", "\\1", last)), 0L)
})
