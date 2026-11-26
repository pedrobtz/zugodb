# Arrow C streams into tables (design §7.3, §6; roadmap Stage 7). The
# oracles: what dbWriteTable() makes of the same data frame (a second
# route to one value), the input itself read back with identical(), and
# the scripted producer's own count of its releases.

test_that("each type with NULLs, through a stream, is the table dbWriteTable() makes", {
  skip_if_not_installed("nanoarrow")
  skip_if_not_installed("bit64")
  skip_if_not_installed("blob")
  con <- DBI::dbConnect(zugodb(), bigint = "integer64")
  withr::defer(DBI::dbDisconnect(con))
  value <- data.frame(
    lgl = c(TRUE, NA, FALSE, TRUE),
    int = c(1L, NA, -2147483647L, 2147483647L),
    dbl = c(1.5, NA, -Inf, 1e-300),
    chr = c("été", NA, "", strrep("long string ", 3)),
    day = as.Date(c("2020-02-29", NA, "1900-01-01", "9999-12-31")),
    ts = as.POSIXct(
      c(1.5, NA, -86400.25, 4102444800.000001),
      origin = "1970-01-01",
      tz = "UTC"
    )
  )
  value$i64 <- bit64::as.integer64(c(
    "9007199254740993",
    NA,
    "-9223372036854775807",
    "0"
  ))
  value$bin <- blob::blob(as.raw(1:3), NULL, raw(), as.raw(0))
  DBI::dbWriteTable(con, "by_frame", value)
  DBI::dbWriteTableArrow(
    con,
    "by_stream",
    nanoarrow::as_nanoarrow_array_stream(value)
  )
  expect_identical(
    DBI::dbGetQuery(con, "DESCRIBE by_stream"),
    DBI::dbGetQuery(con, "DESCRIBE by_frame")
  )
  expect_identical(
    DBI::dbReadTable(con, "by_stream"),
    DBI::dbReadTable(con, "by_frame")
  )
  # A data frame is converted by nanoarrow, as DBI's default would.
  DBI::dbWriteTableArrow(con, "by_convert", value)
  expect_identical(
    DBI::dbReadTable(con, "by_convert"),
    DBI::dbReadTable(con, "by_frame")
  )
})

test_that("NaN is a value and a NULL is NA, through a stream", {
  skip_if_not_installed("nanoarrow")
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  # nanoarrow makes NaN a null from R; this array has NaN as a value.
  dbl <- raw_array(nanoarrow::na_double(), writeBin(c(NaN, 1, -0), raw()), 3)
  DBI::dbWriteTableArrow(con, "t", struct_stream(list(x = dbl)))
  got <- DBI::dbReadTable(con, "t")$x
  expect_identical(got, c(NaN, 1, -0))
  expect_identical(1 / got[[3]], -Inf)
  DBI::dbAppendTableArrow(con, "t", data.frame(x = NA_real_))
  expect_identical(
    DBI::dbGetQuery(con, "SELECT count(*) AS n FROM t WHERE x IS NULL")$n,
    1
  )
})

test_that("batches across a chunk and a row group read back identical", {
  skip_if_not_installed("nanoarrow")
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  frame <- function(n) {
    k <- seq_len(n)
    # NULLs on each side of the 2048-row chunk and the 122,880-row group
    # (the class of #45), and at the last row.
    nul <- k %in% c(2047L, 2048L, 2049L, 122879L, 122880L, 122881L, n)
    data.frame(
      k = k,
      v = ifelse(nul, NA, k * 0.5),
      s = ifelse(nul, NA, paste0("s", k)),
      b = ifelse(nul, NA, k %% 2L == 0L)
    )
  }
  for (n in c(2047L, 2048L, 2049L, 122879L, 122880L, 122881L)) {
    value <- frame(n)
    DBI::dbWriteTableArrow(con, "t", value, overwrite = TRUE)
    expect_identical(
      DBI::dbGetQuery(con, "SELECT * FROM t ORDER BY k"),
      value,
      label = n
    )
  }
  # Batches of uneven lengths, so chunks do not line up with them.
  value <- frame(6145L)
  cuts <- list(1:2047, 2048:4096, 4097L, 4098:6145)
  batches <- lapply(cuts, function(r) {
    part <- value[r, ]
    rownames(part) <- NULL
    nanoarrow::as_nanoarrow_array(part)
  })
  stream <- nanoarrow::basic_array_stream(batches)
  DBI::dbWriteTableArrow(con, "u", stream)
  expect_identical(DBI::dbGetQuery(con, "SELECT * FROM u ORDER BY k"), value)
})

test_that("a column of a type the engine cannot hold is refused from the schema", {
  skip_if_not_installed("nanoarrow")
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  ext <- nanoarrow::nanoarrow_schema_modify(
    nanoarrow::na_int32(),
    list(metadata = list("ARROW:extension:name" = "an.extension"))
  )
  refused <- list(
    dec = nanoarrow::na_decimal128(10, 2),
    clock = nanoarrow::na_time64("us"),
    items = nanoarrow::na_list(nanoarrow::na_int32()),
    rec = nanoarrow::na_struct(list(a = nanoarrow::na_int32())),
    dict = nanoarrow::na_dictionary(nanoarrow::na_string()),
    span = nanoarrow::na_duration("s"),
    months = nanoarrow::na_interval_months(),
    pairs = nanoarrow::na_map(
      nanoarrow::na_string(nullable = FALSE),
      nanoarrow::na_int32()
    ),
    half = nanoarrow::na_half_float(),
    nothing = nanoarrow::na_na(),
    extended = ext
  )
  DBI::dbExecute(con, "CREATE TABLE have (ok INTEGER)")
  for (col in names(refused)) {
    types <- list(ok = nanoarrow::na_int32())
    types[[col]] <- refused[[col]]
    err <- expect_error(
      DBI::dbWriteTableArrow(con, "t", empty_stream(types)),
      class = "zug_type_error",
      label = col
    )
    expect_match(
      conditionMessage(err),
      sprintf("Column `%s`", col),
      fixed = TRUE
    )
    expect_false(DBI::dbExistsTable(con, "t"), label = col)
    expect_error(
      DBI::dbCreateTableArrow(con, "t", nanoarrow::na_struct(types)),
      class = "zug_type_error",
      label = col
    )
    expect_false(DBI::dbExistsTable(con, "t"), label = col)
    # An append to a table with that column is refused before any row.
    DBI::dbExecute(con, sprintf('ALTER TABLE have ADD COLUMN "%s" DOUBLE', col))
    err <- expect_error(
      DBI::dbAppendTableArrow(con, "have", empty_stream(types)),
      class = "zug_type_error",
      label = col
    )
    expect_match(
      conditionMessage(err),
      sprintf("Column `%s`", col),
      fixed = TRUE
    )
  }
  expect_identical(tables_of(con), "have")
})

test_that("a producer that fails at its third batch leaves nothing, by every route", {
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  failing <- function() {
    zug_scripted_stream(
      5,
      1000,
      fail_at = 3,
      message = "the third batch is lost"
    )
  }
  expect_failure_leaves <- function(write, route) {
    before <- zug_scripted_releases()
    err <- expect_error(
      write(failing()),
      class = "zug_stream_error",
      label = route
    )
    expect_match(conditionMessage(err), "the third batch is lost", fixed = TRUE)
    # Released once, by the consumer.
    expect_identical(zug_scripted_releases() - before, 1L, label = route)
  }
  # A new table: removed.
  expect_failure_leaves(function(s) DBI::dbWriteTableArrow(con, "t", s), "new")
  expect_identical(tables_of(con), character())
  # An append: the table as it was, and no staging table left.
  DBI::dbWriteTableArrow(con, "t", zug_scripted_stream(1, 7))
  kept <- DBI::dbReadTable(con, "t")
  expect_failure_leaves(
    function(s) DBI::dbAppendTableArrow(con, "t", s),
    "append"
  )
  expect_failure_leaves(
    function(s) DBI::dbWriteTableArrow(con, "t", s, append = TRUE),
    "write, append"
  )
  # An overwrite: the old table kept.
  expect_failure_leaves(
    function(s) DBI::dbWriteTableArrow(con, "t", s, overwrite = TRUE),
    "write, overwrite"
  )
  expect_identical(DBI::dbReadTable(con, "t"), kept)
  expect_identical(tables_of(con), "t")
})

test_that("a value its column cannot hold, in a later batch, leaves the table as it was", {
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  DBI::dbExecute(con, "CREATE TABLE t (x TINYINT, s VARCHAR)")
  DBI::dbExecute(con, "INSERT INTO t VALUES (1, 'a')")
  before <- zug_scripted_releases()
  # Rows 1 to 100 fit a TINYINT; row 128, in the second batch, does not.
  err <- expect_error(
    DBI::dbAppendTableArrow(con, "t", zug_scripted_stream(3, 100)),
    class = "zug_type_error"
  )
  expect_match(conditionMessage(err), "Column `x`, row 128", fixed = TRUE)
  expect_identical(zug_scripted_releases() - before, 1L)
  expect_identical(DBI::dbGetQuery(con, "SELECT count(*) AS n FROM t")$n, 1)
  expect_identical(tables_of(con), "t")
})

test_that("an append matches columns by name and converts as dbAppendTable() does", {
  skip_if_not_installed("nanoarrow")
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  DBI::dbExecute(
    con,
    "CREATE TABLE t (a BIGINT, b INTEGER, c DOUBLE, d VARCHAR)"
  )
  value <- data.frame(d = c("x", "y"), b = c(3, -4), a = 1:2)
  expect_identical(DBI::dbAppendTableArrow(con, "t", value), 2)
  expect_identical(DBI::dbAppendTable(con, "t", value), 2)
  got <- DBI::dbGetQuery(con, "SELECT * FROM t")
  expect_identical(got[1:2, ], got[3:4, ], ignore_attr = TRUE)
  expect_identical(got$c, rep(NA_real_, 4))
  expect_identical(got$b, c(3L, -4L, 3L, -4L))
  expect_error(
    DBI::dbAppendTableArrow(con, "t", data.frame(b = 0.5)),
    class = "zug_type_error"
  )
  expect_error(
    DBI::dbAppendTableArrow(con, "t", data.frame(zz = 1L)),
    class = "zug_argument_error"
  )
  expect_error(
    DBI::dbAppendTableArrow(con, "t", data.frame(d = 1L)),
    class = "zug_type_error"
  )
  expect_error(
    DBI::dbAppendTableArrow(con, "nope", data.frame(a = 1L)),
    class = "zug_argument_error"
  )
  expect_identical(DBI::dbGetQuery(con, "SELECT count(*) AS n FROM t")$n, 4)
})

test_that("an overwrite replaces the table, and an existing table needs one of the flags", {
  skip_if_not_installed("nanoarrow")
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  DBI::dbWriteTable(con, "t", data.frame(old = 1:3))
  expect_error(
    DBI::dbWriteTableArrow(con, "t", data.frame(new = "x")),
    class = "zug_argument_error"
  )
  expect_error(
    DBI::dbWriteTableArrow(con, "T", data.frame(new = "x")),
    class = "zug_argument_error"
  )
  expect_error(
    DBI::dbWriteTableArrow(
      con,
      "t",
      data.frame(new = "x"),
      overwrite = TRUE,
      append = TRUE
    ),
    class = "zug_argument_error"
  )
  DBI::dbWriteTableArrow(
    con,
    "t",
    data.frame(new = c("x", "y")),
    overwrite = TRUE
  )
  expect_identical(DBI::dbReadTable(con, "t"), data.frame(new = c("x", "y")))
  DBI::dbWriteTableArrow(con, "t", data.frame(new = "z"), append = TRUE)
  expect_identical(DBI::dbReadTable(con, "t")$new, c("x", "y", "z"))
  expect_identical(tables_of(con), "t")
  expect_error(
    DBI::dbWriteTableArrow(con, "u", data.frame(a = 1L), temporary = TRUE),
    class = "zug_unsupported"
  )
  expect_error(
    DBI::dbWriteTableArrow(con, "u", data.frame(a = 1L, A = 2L)),
    class = "zug_argument_error"
  )
  expect_false(DBI::dbExistsTable(con, "u"))
})

test_that("dbCreateTableArrow() makes an empty table of the schema's types", {
  skip_if_not_installed("nanoarrow")
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  na <- asNamespace("nanoarrow")
  types <- list(
    b = na$na_bool(),
    i8 = na$na_int8(),
    i16 = na$na_int16(),
    i32 = na$na_int32(),
    i64 = na$na_int64(),
    u8 = na$na_uint8(),
    u16 = na$na_uint16(),
    u32 = na$na_uint32(),
    u64 = na$na_uint64(),
    f = na$na_float(),
    g = na$na_double(),
    s = na$na_string(),
    ls = na$na_large_string(),
    sv = na$na_string_view(),
    z = na$na_binary(),
    lz = na$na_large_binary(),
    zv = na$na_binary_view(),
    w = na$na_fixed_size_binary(4),
    d32 = na$na_date32(),
    d64 = na$na_date64(),
    tss = na$na_timestamp("s"),
    tsm = na$na_timestamp("ms", "UTC"),
    tsu = na$na_timestamp("us", "Europe/Lisbon"),
    tsn = na$na_timestamp("ns")
  )
  DBI::dbCreateTableArrow(con, "t", na$na_struct(types))
  got <- DBI::dbGetQuery(con, "DESCRIBE t")
  expect_identical(got$column_name, names(types))
  expect_identical(
    got$column_type,
    c(
      "BOOLEAN",
      "TINYINT",
      "SMALLINT",
      "INTEGER",
      "BIGINT",
      "SMALLINT",
      "INTEGER",
      "BIGINT",
      "BIGINT",
      "FLOAT",
      "DOUBLE",
      "VARCHAR",
      "VARCHAR",
      "VARCHAR",
      "BLOB",
      "BLOB",
      "BLOB",
      "BLOB",
      "DATE",
      "DATE",
      "TIMESTAMP",
      "TIMESTAMP",
      "TIMESTAMP",
      "TIMESTAMP"
    )
  )
  expect_identical(DBI::dbGetQuery(con, "SELECT count(*) AS n FROM t")$n, 0)
  # A data frame's schema, as nanoarrow infers it; an existing table.
  DBI::dbCreateTableArrow(con, "u", data.frame(x = 1L, y = "a"))
  expect_identical(DBI::dbListFields(con, "u"), c("x", "y"))
  expect_error(
    DBI::dbCreateTableArrow(con, "u", data.frame(x = 1L)),
    class = "zug_argument_error"
  )
})

test_that("strings and binaries of every layout read back as written", {
  skip_if_not_installed("nanoarrow")
  skip_if_not_installed("blob")
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  s <- c("short", "a string longer than twelve bytes", NA, "", "é中")
  bins <- lapply(s, function(x) if (is.na(x)) NULL else charToRaw(enc2utf8(x)))
  value <- data.frame(s = s, ls = s, sv = s)
  value$z <- value$lz <- value$zv <- blob::as_blob(bins)
  value <- value[c("s", "ls", "sv", "z", "lz", "zv")]
  schema <- nanoarrow::na_struct(list(
    s = nanoarrow::na_string(),
    ls = nanoarrow::na_large_string(),
    sv = nanoarrow::na_string_view(),
    z = nanoarrow::na_binary(),
    lz = nanoarrow::na_large_binary(),
    zv = nanoarrow::na_binary_view()
  ))
  stream <- nanoarrow::as_nanoarrow_array_stream(value, schema = schema)
  DBI::dbWriteTableArrow(con, "t", stream)
  expect_identical(
    DBI::dbGetQuery(con, "DESCRIBE t")$column_type,
    rep(c("VARCHAR", "BLOB"), each = 3)
  )
  got <- DBI::dbReadTable(con, "t")
  for (k in c("s", "ls", "sv")) {
    expect_identical(got[[k]], s, label = k)
  }
  for (k in c("z", "lz", "zv")) {
    expect_identical(unclass(got[[k]]), bins, ignore_attr = TRUE, label = k)
  }
  fixed <- raw_array(nanoarrow::na_fixed_size_binary(3), as.raw(1:6), 2)
  DBI::dbWriteTableArrow(con, "w", struct_stream(list(w = fixed)))
  expect_identical(
    unclass(DBI::dbReadTable(con, "w")$w),
    list(as.raw(1:3), as.raw(4:6)),
    ignore_attr = TRUE
  )
})

test_that("a string that is not UTF-8, or holds a NUL, is a type error naming its row", {
  skip_if_not_installed("nanoarrow")
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  bad <- function(bytes) {
    a <- nanoarrow::as_nanoarrow_array(c("ok", "xy"))
    nanoarrow::nanoarrow_array_modify(
      a,
      list(
        buffers = list(
          NULL,
          a$buffers[[2]],
          nanoarrow::as_nanoarrow_buffer(c(charToRaw("ok"), bytes))
        )
      )
    )
  }
  for (bytes in list(
    as.raw(c(0x61, 0xff)),
    as.raw(c(0x61, 0x00)),
    as.raw(c(0xc0, 0x80))
  )) {
    err <- expect_error(
      DBI::dbWriteTableArrow(con, "t", struct_stream(list(s = bad(bytes)))),
      class = "zug_type_error"
    )
    expect_match(conditionMessage(err), "Column `s`, row 2", fixed = TRUE)
    expect_false(DBI::dbExistsTable(con, "t"))
  }
  # The same bytes in a binary column are a value.
  blob <- nanoarrow::as_nanoarrow_array(
    list(as.raw(c(0x61, 0x00, 0xff))),
    schema = nanoarrow::na_binary()
  )
  DBI::dbWriteTableArrow(con, "b", struct_stream(list(b = blob)))
  expect_identical(
    DBI::dbReadTable(con, "b")$b[[1]],
    as.raw(c(0x61, 0x00, 0xff))
  )
})

test_that("unsigned integers widen, and a uint64 past 2^63 - 1 is refused", {
  skip_if_not_installed("nanoarrow")
  skip_if_not_installed("bit64")
  con <- DBI::dbConnect(zugodb(), bigint = "character")
  withr::defer(DBI::dbDisconnect(con))
  cols <- list(
    # From bytes: nanoarrow needs arrow to make unsigned arrays from R.
    u8 = raw_array(nanoarrow::na_uint8(), as.raw(c(0, 255)), 2),
    u16 = raw_array(
      nanoarrow::na_uint16(),
      writeBin(c(0L, 65535L), raw(), size = 2, endian = "little"),
      2
    ),
    # -1 as four bytes is 2^32 - 1 unsigned.
    u32 = raw_array(
      nanoarrow::na_uint32(),
      writeBin(c(0L, -1L), raw(), size = 4, endian = "little"),
      2
    ),
    u64 = raw_array(
      nanoarrow::na_uint64(),
      u64_bytes(c("7ffffffffffffffe", "7fffffffffffffff")),
      2
    )
  )
  DBI::dbWriteTableArrow(con, "t", struct_stream(cols))
  got <- DBI::dbReadTable(con, "t")
  expect_identical(got$u8, c(0L, 255L))
  expect_identical(got$u16, c(0L, 65535L))
  expect_identical(got$u32, c("0", "4294967295"))
  expect_identical(got$u64, c("9223372036854775806", "9223372036854775807"))
  big <- raw_array(
    nanoarrow::na_uint64(),
    u64_bytes(c("1", "8000000000000000")),
    2
  )
  err <- expect_error(
    DBI::dbWriteTableArrow(con, "u", struct_stream(list(u64 = big))),
    class = "zug_type_error"
  )
  expect_match(conditionMessage(err), "Column `u64`, row 2", fixed = TRUE)
  expect_false(DBI::dbExistsTable(con, "u"))
})

test_that("an append straddles each integer column's range and 2^53 for a double", {
  skip_if_not_installed("nanoarrow")
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  DBI::dbExecute(
    con,
    "CREATE TABLE t (t8 TINYINT, t16 SMALLINT, t32 INTEGER, d DOUBLE, f FLOAT)"
  )
  one <- function(col, x, type) {
    cols <- list(nanoarrow::as_nanoarrow_array(x, schema = type))
    names(cols) <- col
    struct_stream(cols)
  }
  i64 <- nanoarrow::na_int64()
  fits <- list(
    t8 = c(-128, 127),
    t16 = c(-32768, 32767),
    t32 = c(-2147483648, 2147483647),
    d = c(-2^53, 2^53)
  )
  misfits <- list(
    t8 = c(-129, 128),
    t16 = c(-32769, 32768),
    t32 = c(-2147483649, 2147483648),
    d = c(-2^53 - 2, 2^53 + 2)
  )
  for (col in names(fits)) {
    expect_identical(
      DBI::dbAppendTableArrow(con, "t", one(col, fits[[col]], i64)),
      2,
      label = col
    )
    for (x in misfits[[col]]) {
      expect_error(
        DBI::dbAppendTableArrow(con, "t", one(col, x, i64)),
        class = "zug_type_error",
        label = paste(col, x)
      )
    }
  }
  expect_error(
    DBI::dbAppendTableArrow(con, "t", one("f", 1e39, nanoarrow::na_double())),
    class = "zug_type_error"
  )
  expect_identical(
    DBI::dbAppendTableArrow(con, "t", one("f", 3e38, nanoarrow::na_double())),
    1
  )
  expect_identical(DBI::dbGetQuery(con, "SELECT count(*) AS n FROM t")$n, 9)
})

test_that("timestamps of each unit and dates of each width are the same instant", {
  skip_if_not_installed("nanoarrow")
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  skip_if_not_installed("bit64")
  # An array of `type` whose int64 values are `values`, decimal strings.
  int64_array <- function(type, values) {
    bytes <- writeBin(unclass(bit64::as.integer64(values)), raw())
    raw_array(type, bytes, length(values))
  }
  ts <- function(unit, values) {
    int64_array(nanoarrow::na_timestamp(unit, "UTC"), values)
  }
  cols <- list(
    s = nanoarrow::as_nanoarrow_array(
      as.POSIXct(c(-1, 1e9), tz = "UTC"),
      schema = nanoarrow::na_timestamp("s", "UTC")
    ),
    ms = nanoarrow::as_nanoarrow_array(
      as.POSIXct(c(-0.001, 1e9 + 0.25), tz = "UTC"),
      schema = nanoarrow::na_timestamp("ms")
    ),
    us = nanoarrow::as_nanoarrow_array(
      as.POSIXct(c(-1e-6, 1e9 + 0.5), tz = "UTC"),
      schema = nanoarrow::na_timestamp("us", "America/New_York")
    ),
    ns = nanoarrow::as_nanoarrow_array(
      as.POSIXct(c(-1e-6, 1e9 + 0.75), tz = "UTC"),
      schema = nanoarrow::na_timestamp("ns")
    )
  )
  DBI::dbWriteTableArrow(con, "t", struct_stream(cols))
  got <- DBI::dbReadTable(con, "t")
  utc <- function(x) as.POSIXct(x, origin = "1970-01-01", tz = "UTC")
  expect_identical(got$s, utc(c(-1, 1e9)))
  expect_identical(got$ms, utc(c(-0.001, 1e9 + 0.25)))
  expect_equal(got$us, utc(c(-1e-6, 1e9 + 0.5)), tolerance = 0)
  expect_equal(got$ns, utc(c(-1e-6, 1e9 + 0.75)), tolerance = 0)
  # A fraction of a microsecond is refused, not rounded: 999, 1000, 1001 ns.
  expect_error(
    DBI::dbWriteTableArrow(con, "n", struct_stream(list(t = ts("ns", "999")))),
    class = "zug_type_error"
  )
  DBI::dbWriteTableArrow(con, "n", struct_stream(list(t = ts("ns", "1000"))))
  expect_identical(DBI::dbReadTable(con, "n")$t, utc(1e-6))
  expect_error(
    DBI::dbWriteTableArrow(
      con,
      "n2",
      struct_stream(list(t = ts("ns", "1001")))
    ),
    class = "zug_type_error"
  )
  # Seconds past TIMESTAMP's range overflow when scaled.
  expect_error(
    DBI::dbWriteTableArrow(
      con,
      "n3",
      struct_stream(list(t = ts("s", "9223372036855")))
    ),
    class = "zug_type_error"
  )
  dates <- list(
    d32 = nanoarrow::as_nanoarrow_array(
      as.Date(c("1969-12-31", "2024-02-29")),
      schema = nanoarrow::na_date32()
    ),
    d64 = nanoarrow::as_nanoarrow_array(
      as.Date(c("1969-12-31", "2024-02-29")),
      schema = nanoarrow::na_date64()
    )
  )
  DBI::dbWriteTableArrow(con, "d", struct_stream(dates))
  got <- DBI::dbReadTable(con, "d")
  expect_identical(got$d32, as.Date(c("1969-12-31", "2024-02-29")))
  expect_identical(got$d64, got$d32)
  part_day <- function(ms) int64_array(nanoarrow::na_date64(), ms)
  expect_error(
    DBI::dbWriteTableArrow(
      con,
      "d2",
      struct_stream(list(d = part_day("86400001")))
    ),
    class = "zug_type_error"
  )
  DBI::dbWriteTableArrow(
    con,
    "d3",
    struct_stream(list(d = part_day("86400000")))
  )
  expect_identical(DBI::dbReadTable(con, "d3")$d, as.Date("1970-01-02"))
})

test_that("a batch with a null row is a stream error", {
  skip_if_not_installed("nanoarrow")
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  arr <- struct_array(list(x = nanoarrow::as_nanoarrow_array(1:3)))
  arr <- nanoarrow::nanoarrow_array_modify(
    arr,
    list(
      null_count = 1,
      buffers = list(nanoarrow::as_nanoarrow_buffer(as.raw(0x05)))
    )
  )
  stream <- nanoarrow::basic_array_stream(
    list(arr),
    schema = nanoarrow::infer_nanoarrow_schema(arr),
    validate = FALSE
  )
  expect_error(
    DBI::dbWriteTableArrow(con, "t", stream),
    class = "zug_stream_error"
  )
  expect_false(DBI::dbExistsTable(con, "t"))
})
