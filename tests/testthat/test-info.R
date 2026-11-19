# zuslothdb_info() reports what the library was compiled from. Its oracle is
# tools/vendor.yaml, installed as vendor.yaml: the commit, tag and origin
# there were compiled in through src/sdb_provenance.h, and a vendor.yaml
# edited without re-running tools/vendor.R makes them disagree.

test_that("zuslothdb_info() reports libzugodb's tag and SlothDB's commit", {
  skip_if_not_installed("yaml")
  path <- system.file("vendor.yaml", package = "zuslothdb")
  expect_true(nzchar(path))
  cfg <- yaml::read_yaml(path)
  info <- zuslothdb_info()

  expect_identical(info$engine, "libzugodb")
  expect_identical(info$repository, cfg$source$repository)
  expect_identical(info$commit, cfg$source$commit)
  expect_match(info$commit, "^[0-9a-f]{40}$")
  expect_identical(info$tag, cfg$source$tag)
  expect_match(info$tag, "^v[0-9]+[.][0-9]+[.][0-9]+$")
  expect_identical(info$origin, cfg$origin$repository)
  expect_identical(info$origin_commit, cfg$origin$commit)
  expect_match(info$origin_commit, "^[0-9a-f]{40}$")
  expect_identical(info$describe, cfg$origin$describe)
  expect_null(info$changes)
})

test_that("the engine runs two threads unless told otherwise (D5)", {
  expect_identical(zuslothdb_info()$threads, 2L)
})
