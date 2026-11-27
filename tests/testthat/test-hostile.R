# Hostile input: a damaged database file (below) and deep expressions
# (at the end).
#
# A damaged database file is an error, not wrong rows (design §8; roadmap
# Stage 6). libzugodb 0.2.0 checks the header and a CRC-32 over the whole
# file before any table is read, and Checkpoint::Load bounds every length
# and count. Each damaged copy of the golden file below is refused with a
# classed error, left as it was (a refused open writes nothing), and
# another database then opens. A one-bit flip anywhere changes the CRC-32,
# so no flip can pass the check.

test_that("a database file with one bit flipped is refused, at every offset tried", {
  flip <- function(golden, off) {
    lapply(c(0L, 7L), function(bit) {
      b <- golden
      b[off + 1L] <- xor(b[off + 1L], as.raw(bitwShiftL(1L, bit)))
      b
    })
  }
  expect_gt(hostile_run(flip), 300L)
})

test_that("a truncated database file is refused, at every length tried", {
  cut <- function(golden, off) {
    list(golden[seq_len(off)])
  }
  expect_gt(hostile_run(cut), 150L)
})

test_that("a file extended past its recorded size is refused", {
  dir <- withr::local_tempdir()
  golden <- read_bytes(golden_copy(dir))
  path <- file.path(dir, "long.zugodb")
  for (extra in list(as.raw(0), as.raw(rep(0, 4096)), golden[1:48])) {
    bytes <- c(golden, extra)
    writeBin(bytes, path)
    expect_refused_file(path, bytes, sprintf("%d extra bytes", length(extra)))
  }
})

test_that("a file that is not a database is refused, an empty one too", {
  dir <- withr::local_tempdir()
  path <- file.path(dir, "x.zugodb")
  for (bytes in list(
    raw(),
    as.raw(1:47),
    charToRaw(strrep("not a database file\n", 300)),
    as.raw(rep(0, 8192))
  )) {
    writeBin(bytes, path)
    expect_refused_file(path, bytes, sprintf("%d bytes", length(bytes)))
  }
})

# Deep expressions (design §10; roadmap Stage 9). libzugodb 0.4.1 bounds
# an expression's height at 256: a leaf is 1, each operator one more, a
# subquery one more than the tallest expression in it. Before it, a chain
# of 5,000 `+` overflowed the C stack and killed R (libzugodb#65).

test_that("the deepest expression the parser takes answers, one level past is a zug_sql_error", {
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  # Each form at height 256 with its answer, and at 257.
  forms <- list(
    plus = list(function(n) sql_chain("1", "+", "1", n - 1L), 256),
    times = list(function(n) sql_chain("1", "*", "1", n - 1L), 1),
    concat = list(
      function(n) sql_chain("'a'", "||", "'a'", n - 1L),
      strrep("a", 256)
    ),
    and = list(function(n) sql_chain("TRUE", "AND", "TRUE", n - 1L), TRUE),
    or = list(function(n) sql_chain("FALSE", "OR", "FALSE", n - 1L), FALSE),
    minus = list(function(n) paste0(strrep("- ", n - 1L), "1"), -1),
    not = list(function(n) paste0(strrep("NOT ", n - 1L), "TRUE"), FALSE),
    cast = list(function(n) paste0("7", strrep("::INTEGER", n - 1L)), 7),
    # Parentheses make no node: 255 pairs are the 256th level.
    parens = list(
      function(n) paste0(strrep("(", n - 1L), "1", strrep(")", n - 1L)),
      1
    ),
    # (SELECT chain of 201) is 202 high; 54 more `+` make 256.
    subquery = list(
      function(n) {
        sql_chain(
          paste0("(SELECT ", sql_chain("1", "+", "1", 200L), ")"),
          "+",
          "1",
          n - 202L
        )
      },
      255
    )
  )
  for (nm in names(forms)) {
    make <- forms[[nm]][[1L]]
    got <- query_or_condition(con, paste("SELECT", make(256L), "AS a"))
    expect_equal(got, forms[[nm]][[2L]], label = nm)
    got <- query_or_condition(con, paste("SELECT", make(257L), "AS a"))
    expect_s3_class(got, "zug_sql_error")
    expect_identical(DBI::dbGetQuery(con, "SELECT 1 AS one")$one, 1L)
  }
})

test_that("an expression 100,000 levels deep is a zug_sql_error, not a crash", {
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  DBI::dbWriteTable(con, "t", data.frame(x = 1:3))
  n <- 100000L
  deep <- c(
    paste("SELECT", sql_chain("1", "+", "1", n)),
    paste("SELECT count(*) FROM t WHERE", sql_chain("x > 0", "AND", "x > 0", n)),
    paste0("SELECT ", strrep("- ", n), "1"),
    paste0("SELECT ", strrep("NOT ", n), "TRUE"),
    paste0("SELECT ", strrep("(", n), "1", strrep(")", n))
  )
  for (sql in deep) {
    expect_error(DBI::dbGetQuery(con, sql), class = "zug_sql_error")
  }
  expect_identical(DBI::dbGetQuery(con, "SELECT count(*) AS n FROM t")$n, 3)
})
