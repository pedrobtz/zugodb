# Operators filled past one 2048-row chunk (design §4.2, #45; roadmap
# Stage 8). libzugodb's Stage 10 audited every chunk the engine makes (L27
# there, 88 sites) with C++ tests; these ask the same of the vendored engine
# through the driver. Each chunk- query runs one operator over exactly n
# rows, for n of 2047, 2048, 2049, 4095, 4096 and 4097, with NULLs on each
# side of both boundaries, and its summary is compared with duckdb's
# recorded answer.

test_that("every chunk- query agrees with duckdb's recorded answer", {
  names <- oracle_names("chunk-")
  expect_gt(length(names), 10)
  for (name in names) {
    want <- oracle_answer(name)
    expect_true(is.data.frame(want), label = name)
    expect_identical(zugodb_answer(name), want, label = name)
  }
})

test_that("each summary straddles both boundaries", {
  for (name in setdiff(oracle_names("chunk-"), "chunk-fetch")) {
    expect_identical(
      oracle_answer(name)$n,
      c(2047L, 2048L, 2049L, 4095L, 4096L, 4097L),
      label = name
    )
  }
})

test_that("the rows each side of a boundary arrive as stored", {
  got <- zugodb_answer("chunk-fetch")
  nulls <- got$i %in% c(2048L, 2049L, 4096L, 4097L)
  expect_true(all(is.na(got$v[nulls])) && all(is.na(got$s[nulls])))
  expect_identical(got$v[!nulls], got$i[!nulls])
  expect_identical(got$s[!nulls], as.character(got$i[!nulls]))
})
