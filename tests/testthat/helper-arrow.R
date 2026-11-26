# Arrow C streams for test-arrow-stream.R and test-lifecycle.R, made with
# nanoarrow (Suggests) or with the package's scripted producer
# (src/zug_stream.cpp, zug_scripted_stream()).

# A stream of one batch holding the columns `cols`, a named list of
# nanoarrow arrays of one length.
struct_stream <- function(cols) {
  arr <- struct_array(cols)
  nanoarrow::basic_array_stream(
    list(arr),
    schema = nanoarrow::infer_nanoarrow_schema(arr),
    validate = FALSE
  )
}

struct_array <- function(cols) {
  schema <- nanoarrow::na_struct(lapply(
    cols,
    nanoarrow::infer_nanoarrow_schema
  ))
  n <- if (length(cols)) cols[[1L]]$length else 0L
  nanoarrow::nanoarrow_array_modify(
    nanoarrow::nanoarrow_array_init(schema),
    list(length = n, null_count = 0, children = cols)
  )
}

# A fixed-width array of `type` with `bytes` as its data and no NULL.
raw_array <- function(type, bytes, length) {
  nanoarrow::nanoarrow_array_modify(
    nanoarrow::nanoarrow_array_init(type),
    list(
      length = length,
      null_count = 0,
      buffers = list(NULL, nanoarrow::as_nanoarrow_buffer(bytes))
    )
  )
}

# The little-endian bytes of unsigned 64-bit integers given as strings of
# hexadecimal digits ("7fffffffffffffff").
u64_bytes <- function(hex) {
  unlist(lapply(hex, function(h) {
    h <- paste0(strrep("0", 16L - nchar(h)), h)
    rev(as.raw(strtoi(substring(h, seq(1, 15, 2), seq(2, 16, 2)), 16L)))
  }))
}

# An empty stream whose schema has the columns `types`, a named list of
# nanoarrow types.
empty_stream <- function(types) {
  nanoarrow::basic_array_stream(list(), schema = nanoarrow::na_struct(types))
}

# The tables a connection has, as a set.
tables_of <- function(con) sort(DBI::dbListTables(con))
