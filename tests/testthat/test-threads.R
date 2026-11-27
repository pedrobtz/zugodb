# One thread and two give the same results (design D5, Q18; roadmap
# Stage 8). Since libzugodb 0.4.0 removed the file readers, the engine's
# one parallel path is the window operator's sort of its partitions, taken
# from 200,000 input rows and two partitions on; every other query runs on
# the query's thread whatever the cap. So the corpus at caps 1 and 2 shows
# the cap changes nothing else, and a partitioned window each side of
# 200,000 rows shows the parallel sort gives the serial one's rows, both
# against an answer computed in R.

test_that("every recorded query gives the same result on one thread and on two", {
  names <- c(
    oracle_names("scalar-"),
    oracle_names("types-"),
    oracle_names("temporal-"),
    oracle_names("sql-"),
    oracle_names("chunk-"),
    oracle_names("diff-")
  )
  expect_gt(length(names), 60)
  for (name in names) {
    expect_identical(
      zugodb_answer(name, threads = 1L),
      zugodb_answer(name, threads = 2L),
      label = name
    )
  }
})

test_that("a partitioned window at 200,000 rows, sorted in parallel, gives the serial rows", {
  df <- window_rows(200000L)
  want <- window_answer(df)
  expect_identical(window_query(df, threads = 2L), want)
  expect_identical(window_query(df, threads = 1L), want)
})

test_that("a partitioned window one row either side of 200,000 gives R's rows", {
  for (n in c(199999L, 200001L)) {
    df <- window_rows(n)
    expect_identical(
      window_query(df, threads = 2L),
      window_answer(df),
      label = n
    )
  }
})
