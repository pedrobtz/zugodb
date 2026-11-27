# The DBI specification, through DBItest (design §10; roadmap Stage 9).
# Every case runs except those in dbitest_skips (helper-dbitest.R), each
# with its reason; the second test holds each skipped case to failing, so
# a fix that makes one pass, or a skip that names no case, shows here.

test_that("zugodb passes DBItest's driver, connection, result and SQL suites", {
  skip_if_not_installed("DBItest")
  ctx <- dbitest_context()
  DBItest::test_all(skip = names(dbitest_skips), ctx = ctx)
})

test_that("every skipped DBItest case still fails, and has a reason", {
  skip_if_not_installed("DBItest")
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
