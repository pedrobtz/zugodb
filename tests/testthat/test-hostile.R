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
