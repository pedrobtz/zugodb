# Stage 1's spike: the engine compiled into the package answers one query,
# through the exception barrier of design §4.3. The oracle is the SQL
# literal itself.

test_that("SELECT 42 returns the integer 42", {
  expect_identical(sdb_query_int("SELECT 42"), 42L)
})

test_that("integer results straddle R's integer range", {
  # 2^31 - 1 is the largest R integer; -2^31 is NA_integer_ in R, so it
  # does not fit either. Each is the literal, or a zuslothdb_type_error.
  expect_identical(sdb_query_int("SELECT 2147483646"), 2147483646L)
  expect_identical(sdb_query_int("SELECT 2147483647"), 2147483647L)
  expect_error(
    sdb_query_int("SELECT 2147483648"),
    class = "zuslothdb_type_error"
  )
  expect_identical(sdb_query_int("SELECT -2147483647"), -2147483647L)
  expect_error(
    sdb_query_int("SELECT -2147483648"),
    class = "zuslothdb_type_error"
  )
})

test_that("a NULL result is NA", {
  expect_identical(sdb_query_int("SELECT NULL"), NA_integer_)
})

test_that("an engine error is a zuslothdb_sql_error and the session goes on", {
  expect_error(sdb_query_int("SELEC 42"), class = "zuslothdb_sql_error")
  expect_error(
    sdb_query_int("SELECT * FROM no_such_table"),
    class = "zuslothdb_sql_error"
  )
  # The exception was caught and its frames unwound: the next query works.
  expect_identical(sdb_query_int("SELECT 41 + 1"), 42L)
})

test_that("a remote path is refused: the package is offline (D4)", {
  for (url in c(
    "http://example.invalid/a.csv",
    "https://example.invalid/a.csv",
    "s3://bucket/a.parquet"
  )) {
    expect_error(
      sdb_query_int(sprintf("SELECT count(*) FROM read_csv('%s')", url)),
      class = "zuslothdb_sql_error"
    )
  }
})

test_that("sql must be one string", {
  for (bad in list(NA_character_, character(), c("SELECT 1", "SELECT 2"), 1)) {
    expect_error(sdb_query_int(bad), class = "zuslothdb_argument_error")
  }
})
