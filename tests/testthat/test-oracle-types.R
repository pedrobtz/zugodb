# BLOB, and Stage 4's types through a set operation's boxed rows, against
# duckdb's recorded answers (design §6; roadmap Stage 4). duckdb returns a
# BLOB as a plain list; zuslothdb gives it blob::blob()'s classes, so the
# values are compared without them and the classes checked on their own
# (strip_blob() and blob_classes in helper-results.R).

test_that("every types- query agrees with duckdb's recorded answer", {
  names <- oracle_names("types-")
  expect_length(names, 2L)
  for (name in names) {
    got <- zuslothdb_answer(name)
    expect_true(is.data.frame(got), label = name)
    expect_identical(strip_blob(got), oracle_answer(name), label = name)
  }
})

test_that("a BLOB column has blob::blob()'s classes and prototype", {
  b <- zuslothdb_answer("types-blob")$b
  expect_identical(class(b), blob_classes)
  expect_identical(attr(b, "ptype"), raw(0))
  expect_null(b[[5]])
  expect_identical(b[[1]], as.raw(c(0xaa, 0x00, 0x62)))
})
