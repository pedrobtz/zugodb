# A table's NULLs past one engine vector (2048 rows), by every insert route
# (#45, patch 0010): the validity mask held 2048 bits, so a NULL from row
# 2049 on was written past a heap buffer and others read back as NULL. The
# INSERT ... SELECT route is also held to duckdb's recorded answer
# (oracle/scalar-nulls-long, in test-oracle-scalar.R).

test_that("a VALUES list longer than a vector keeps its NULLs and values", {
  con <- DBI::dbConnect(zuslothdb())
  withr::defer(DBI::dbDisconnect(con))
  n <- 5000L
  k <- seq_len(n) - 1L
  # NULLs at each side of the vector boundaries and every 997 rows.
  null <- k %% 997L == 5L | k %in% c(2047L, 2048L, 4095L, 4096L, 4999L)
  y <- ifelse(null, NA_integer_, k - 2500L)
  s <- ifelse(null, NA_character_, sprintf("row %d with some text", k))
  values <- paste0(
    "(", k, ", ", ifelse(null, "NULL", y), ", ",
    ifelse(null, "NULL", paste0("'", s, "'")), ")",
    collapse = ", "
  )
  DBI::dbExecute(con, "CREATE TABLE t (k INTEGER, y INTEGER, s VARCHAR)")
  expect_identical(DBI::dbExecute(con, paste("INSERT INTO t VALUES", values)), as.double(n))
  got <- DBI::dbGetQuery(con, "SELECT k, y, s FROM t ORDER BY k")
  expect_identical(got$k, k)
  expect_identical(got$y, y)
  expect_identical(got$s, s)
  expect_identical(
    DBI::dbGetQuery(con, "SELECT k FROM t WHERE y IS NULL ORDER BY k")$k,
    k[null]
  )
})

test_that("a table filled by many inserts keeps its NULLs past a vector", {
  con <- DBI::dbConnect(zuslothdb())
  withr::defer(DBI::dbDisconnect(con))
  DBI::dbExecute(con, "CREATE TABLE t (k INTEGER, y DOUBLE)")
  k <- 0:4199
  null <- k %% 1000L == 999L
  for (part in split(k, k %/% 700L)) {
    values <- paste0(
      "(", part, ", ", ifelse(part %% 1000L == 999L, "NULL", part / 4), ")",
      collapse = ", "
    )
    DBI::dbExecute(con, paste("INSERT INTO t VALUES", values))
  }
  got <- DBI::dbGetQuery(con, "SELECT y FROM t ORDER BY k")$y
  expect_identical(is.na(got), null)
  expect_identical(got[!null], k[!null] / 4)
})
