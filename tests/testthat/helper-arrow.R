# Arrow C streams for test-arrow-stream.R and test-lifecycle.R. Valid
# streams come from zuarrow (Suggests), the family's Arrow layer. The ones
# no producer should make (a string that is not UTF-8, a null row, a batch
# whose layout is not its format's, a producer that fails on cue) come from
# the package's own producers in src/zug_stream.cpp: zug_scripted_stream()
# and zug_raw_stream().

# A data frame of the columns `...`, each of `n` rows, kept as given (a
# list column stays a list, a data frame column a data frame).
frame_of <- function(..., n = NULL) {
  cols <- list(...)
  if (is.null(n)) {
    first <- cols[[1L]]
    n <- if (is.data.frame(first)) nrow(first) else length(first)
  }
  structure(cols, class = "data.frame", row.names = .set_row_names(n))
}

# A zuarrow stream of `df`, its columns of the formats `schema` (a named
# character vector) where given.
zua_stream <- function(df, schema = NULL, batch = 65536L) {
  zuarrow::arrow_stream(df, batch = batch, schema = schema)
}

# Little-endian bytes for zug_raw_stream()'s buffers.
int32_bytes <- function(x) {
  writeBin(as.integer(x), raw(), size = 4, endian = "little")
}

# Whole numbers of magnitude below 2^53 as int64 bytes.
int64_bytes <- function(x) {
  u32 <- function(u) if (u >= 2^31) u - 2^32 else u
  unlist(lapply(x, function(v) {
    lo <- v %% 2^32
    hi <- ((v - lo) / 2^32) %% 2^32
    writeBin(as.integer(c(u32(lo), u32(hi))), raw(), size = 4, endian = "little")
  }))
}

# The little-endian bytes of unsigned 64-bit integers given as hexadecimal
# digits ("7fffffffffffffff").
u64_bytes <- function(hex) {
  unlist(lapply(hex, function(h) {
    h <- paste0(strrep("0", 16L - nchar(h)), h)
    rev(as.raw(strtoi(substring(h, seq(1, 15, 2), seq(2, 16, 2)), 16L)))
  }))
}

# A zug_raw_stream() column of a string or binary format ("u", "z") from a
# list of raw vectors, with no NULL.
bytes_column <- function(format, values) {
  ends <- cumsum(vapply(values, length, 1L))
  list(
    format = format,
    length = length(values),
    buffers = list(NULL, int32_bytes(c(0L, ends)), unlist(values))
  )
}

# A zug_raw_stream() column of a fixed-width format from its data bytes.
fixed_column <- function(format, bytes, length, validity = NULL, null_count = 0) {
  list(
    format = format,
    length = length,
    null_count = null_count,
    buffers = list(validity, bytes)
  )
}

# The tables a connection has, as a set.
tables_of <- function(con) sort(DBI::dbListTables(con))
