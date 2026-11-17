# zuslothdb_info() reports what the library was compiled from. Its oracle is
# tools/vendor.yaml, installed as vendor.yaml: the commit and patch list
# there were compiled in through src/sdb_provenance.h, and a vendor.yaml
# edited without re-running tools/vendor.R makes the two disagree.

test_that("zuslothdb_info() reports the pinned commit and the patch list", {
  skip_if_not_installed("yaml")
  path <- system.file("vendor.yaml", package = "zuslothdb")
  expect_true(nzchar(path))
  cfg <- yaml::read_yaml(path)
  info <- zuslothdb_info()

  expect_identical(info$engine, "SlothDB")
  expect_identical(info$repository, cfg$upstream$repository)
  expect_identical(info$commit, cfg$upstream$commit)
  expect_match(info$commit, "^[0-9a-f]{40}$")
  expect_identical(info$describe, cfg$upstream$describe)
  expect_identical(info$patches, vapply(cfg$patches, `[[`, "", "file"))
})

test_that("the engine runs two threads unless told otherwise (D5)", {
  expect_identical(zuslothdb_info()$threads, 2L)
})
