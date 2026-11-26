# The golden database file (tools/golden.R; design §8, Q7), copied to a
# fresh path so a test never writes the fixture.
golden_copy <- function(dir) {
  to <- file.path(dir, "golden.zugodb")
  stopifnot(file.copy(test_path("fixtures", "golden", "v1001.zugodb"), to))
  to
}

golden_rows <- function() {
  eval(parse(test_path("fixtures", "golden", "v1001.rows.R"), encoding = "UTF-8"))
}

# The table `many`, as tools/golden.R writes it.
golden_many <- function() {
  k <- 0:4999
  data.frame(k = k, v = ifelse(k %% 7L == 0L, NA, k * 3), s = paste0("r", k))
}

read_bytes <- function(path) readBin(path, "raw", file.size(path))

# For test-persist.R: one value of each type written, with NA.
persist_value <- function() {
  data.frame(
    i = c(1L, NA, -2147483647L, 2147483647L),
    d = c(1.5, NA, NaN, -Inf),
    s = c("a", NA, "été 日本", ""),
    b = c(TRUE, NA, FALSE, TRUE),
    dt = as.Date(c("1969-07-20", NA, "2024-02-29", "0001-01-01")),
    ts = as.POSIXct(c(-0.5, NA, 1709208000.000001, 0), origin = "1970-01-01", tz = "UTC"),
    stringsAsFactors = FALSE
  )
}

# For test-hostile.R.
# Offsets spread over the file: every header byte, the zero padding up to
# the tables at 4096, then each part of the data, and the last byte. The
# heavy run adds one offset in every 509 bytes.
hostile_offsets <- function(size, heavy) {
  o <- c(0:63, 4000:4100, size %/% 4, size %/% 2, size - 64:1)
  if (heavy) o <- c(o, seq(4096, size - 1, by = 509))
  sort(unique(o[o >= 0 & o < size]))
}

expect_refused_file <- function(path, bytes, label) {
  err <- expect_error(
    DBI::dbConnect(zugodb(), dbdir = path),
    class = "zug_sql_error",
    label = label
  )
  expect_true(nzchar(conditionMessage(err)), label = label)
  expect_identical(read_bytes(path), bytes, label = label)
}

hostile_run <- function(damage) {
  dir <- withr::local_tempdir()
  golden <- read_bytes(golden_copy(dir))
  heavy <- !identical(tolower(Sys.getenv("ZUG_SKIP_HEAVY")), "true")
  path <- file.path(dir, "damaged.zugodb")
  n <- 0L
  for (off in hostile_offsets(length(golden), heavy)) {
    for (bytes in damage(golden, off)) {
      writeBin(bytes, path)
      expect_refused_file(path, bytes, sprintf("offset %d, %d bytes", off, length(bytes)))
      n <- n + 1L
    }
  }
  # Another database opens, in this process, after every refusal.
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  expect_identical(DBI::dbGetQuery(con, "SELECT 41 + 1 AS x")$x, 42L)
  n
}
