# The DBI specification, through DBItest (design §10; roadmap Stage 9).
# Every case runs except those in dbitest_skips (helper-dbitest.R), each
# with its reason; the second test holds each skipped case to failing, so
# a fix that makes one pass, or a skip that names no case, shows here.
# The timestamp cases in dbitest_tz_cases are skipped only where the
# system has no PST8PDT.

test_that("zugodb passes DBItest's driver, connection, result and SQL suites", {
  skip_if_not_installed("DBItest")
  skip_under_memory_tools()
  skip <- names(dbitest_skips)
  if (!has_pst8pdt()) {
    skip <- c(skip, names(dbitest_tz_cases))
  }
  ctx <- dbitest_context()
  DBItest::test_all(skip = skip, ctx = ctx)
})

test_that("every skipped DBItest case still fails, and has a reason", {
  skip_if_not_installed("DBItest")
  skip_under_memory_tools()
  expect_true(all(nzchar(dbitest_skips)))
  expect_false(anyDuplicated(names(dbitest_skips)) > 0L)
  expect_true(all(nzchar(dbitest_followers)))
  ctx <- dbitest_context()
  for (pattern in names(dbitest_skips)) {
    got <- dbitest_outcomes(ctx, pattern)
    # A pattern names at least one case, and every case it names fails.
    expect_gt(nrow(got), 0L, label = pattern)
    expect_identical(
      setdiff(got$test[!got$failed], names(dbitest_followers)),
      character(),
      label = pattern
    )
  }
})

test_that("the time zone skips name DBItest cases, and pass with PST8PDT", {
  skip_if_not_installed("DBItest")
  skip_under_memory_tools()
  skip_if_not(has_pst8pdt(), "no PST8PDT in this system's time zones")
  expect_true(all(nzchar(dbitest_tz_cases)))
  ctx <- dbitest_context()
  for (pattern in names(dbitest_tz_cases)) {
    got <- dbitest_outcomes(ctx, pattern)
    expect_gt(nrow(got), 0L, label = pattern)
    expect_false(any(got$failed), label = pattern)
  }
})

test_that("has_pst8pdt() is FALSE where the zone is missing", {
  skip_if_not_installed("lubridate")
  expect_true(is.logical(has_pst8pdt()) && length(has_pst8pdt()) == 1L)
  # A zone no tzdata has stands in for a system without PST8PDT.
  testthat::local_mocked_bindings(
    with_tz = function(time, tzone = "", ...) {
      warning("Unrecognized time zone '", tzone, "'")
      time
    },
    .package = "lubridate"
  )
  expect_false(has_pst8pdt())
})
