# Scalar types against duckdb's recorded answers (design §6, §10; roadmap
# Stage 3): each type with NULLs and its extremes, an empty result and one
# longer than an engine chunk. A recorded error means duckdb refused the
# query, and zugodb must refuse it too.

test_that("every scalar query agrees with duckdb's recorded answer", {
  names <- oracle_names("scalar-")
  expect_gt(length(names), 8)
  for (name in names) {
    want <- oracle_answer(name)
    got <- zugodb_answer(name)
    if (!is.data.frame(want)) {
      expect_false(is.data.frame(got), label = name)
    } else {
      expect_identical(got, want, label = name)
    }
  }
})

test_that("each known difference is listed, and still differs", {
  diffs <- oracle_names("diff-")
  expect_setequal(diffs, oracle_known_differences())
  for (name in diffs) {
    want <- oracle_answer(name)
    got <- zugodb_answer(name)
    expect_false(identical(got, want), label = name)
    expect_false(
      is.data.frame(got) == is.data.frame(want) && !is.data.frame(got),
      label = paste(name, "(both errors are not a difference)")
    )
  }
})

test_that("strings arrive marked UTF-8", {
  got <- zugodb_answer("scalar-varchar")$s
  non_ascii <- !is.na(got) & grepl("[^\\x01-\\x7F]", got, perl = TRUE)
  expect_true(any(non_ascii))
  expect_true(all(Encoding(got[non_ascii]) == "UTF-8"))
})
