# zuslothdb_info() reports what the library was compiled from. Its oracle is
# tools/vendor.yaml, installed as vendor.yaml: the commits and the change
# list there were compiled in through src/sdb_provenance.h, and a
# vendor.yaml edited without re-running tools/vendor.R makes them disagree.

test_that("zuslothdb_info() reports the fork's commit and its changes", {
  skip_if_not_installed("yaml")
  path <- system.file("vendor.yaml", package = "zuslothdb")
  expect_true(nzchar(path))
  cfg <- yaml::read_yaml(path)
  info <- zuslothdb_info()

  expect_identical(info$engine, "SlothDB")
  expect_identical(info$repository, cfg$source$repository)
  expect_identical(info$commit, cfg$source$commit)
  expect_match(info$commit, "^[0-9a-f]{40}$")
  expect_identical(info$tag, cfg$source$tag)
  expect_identical(info$upstream, cfg$upstream$repository)
  expect_identical(info$upstream_commit, cfg$upstream$commit)
  expect_match(info$upstream_commit, "^[0-9a-f]{40}$")
  expect_identical(info$describe, cfg$upstream$describe)
  expect_identical(info$changes, vapply(cfg$changes, `[[`, "", "subject"))
  # The fork's last change is the commit vendored.
  expect_identical(cfg$changes[[length(cfg$changes)]]$commit, info$commit)
})

test_that("the engine runs two threads unless told otherwise (D5)", {
  expect_identical(zuslothdb_info()$threads, 2L)
})
