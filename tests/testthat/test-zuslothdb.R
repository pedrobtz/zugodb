# Stage 0's placeholder: the package loads, under the name the design gives
# it (Q8), with its identity filled in rather than the scaffold's text.
# Stage 1 adds the first test of behaviour.

test_that("the package loads with its own name and title", {
  expect_true(isNamespaceLoaded("zuslothdb"))
  desc <- utils::packageDescription("zuslothdb")
  expect_identical(desc$Package, "zuslothdb")
  expect_false(grepl("What the Package Does", desc$Title, fixed = TRUE))
  expect_false(grepl("What the package does", desc$Description, fixed = TRUE))
})
