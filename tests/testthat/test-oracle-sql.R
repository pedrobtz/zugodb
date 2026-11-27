# The core's SQL against duckdb's recorded answers (design §10, Q18;
# roadmap Stage 8): joins, GROUP BY, window functions, subqueries, CTEs,
# set operations and ORDER BY with LIMIT, over NULL keys, repeats and empty
# inputs. The engine's own conformance against DuckDB's sqllogictest corpus
# is libzugodb's (its Stages 6 to 10, counted at the vendored tag); this is
# the same question asked through the driver, so the result conversion is
# in the path.
#
# Q18 allows an error and never a wrong answer: a query duckdb answers and
# the engine refuses is a refused- file, listed in known-differences.md, and
# must still be refused.

test_that("every sql- query agrees with duckdb's recorded answer", {
  names <- oracle_names("sql-")
  expect_gt(length(names), 25)
  for (name in names) {
    want <- oracle_answer(name)
    expect_true(is.data.frame(want), label = paste(name, "(duckdb answers)"))
    expect_identical(zugodb_answer(name), want, label = name)
  }
})

test_that("each refused- query is listed, answered by duckdb, and still refused", {
  names <- oracle_names("refused-")
  expect_setequal(names, oracle_known_differences("refused-"))
  for (name in names) {
    expect_true(is.data.frame(oracle_answer(name)), label = name)
    got <- zugodb_condition(name)
    expect_s3_class(got$condition, "zug_sql_error")
    expect_identical(got$after, 1L, label = paste(name, "(connection after)"))
  }
})
