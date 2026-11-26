# Arrow C streams into tables (design §7.3, §6; roadmap Stage 7). The
# producer is zuarrow, the family's Arrow layer; what no producer should
# make comes from the package's own (helper-arrow.R). The oracles: what
# dbWriteTable() makes of the same data frame (a second route to one
# value), the input itself read back with identical(), and the scripted
# producer's own count of its releases.

test_that("each type with NULLs, through a stream, is the table dbWriteTable() makes", {
  skip_if_not_installed("zuarrow")
  skip_if_not_installed("bit64")
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
  value$bin <- list(as.raw(1:3), NULL, raw(), as.raw(0))
  DBI::dbWriteTable(con, "by_frame", value)
  DBI::dbWriteTableArrow(con, "by_stream", zuarrow::arrow_stream(value))
  expect_identical(
    DBI::dbGetQuery(con, "DESCRIBE by_stream"),
    DBI::dbGetQuery(con, "DESCRIBE by_frame")
  )
  expect_identical(
    DBI::dbReadTable(con, "by_stream"),
    DBI::dbReadTable(con, "by_frame")
  )
  # A data frame is converted by zuarrow.
  DBI::dbWriteTableArrow(con, "by_convert", value)
  expect_identical(
    DBI::dbReadTable(con, "by_convert"),
    DBI::dbReadTable(con, "by_frame")
  )
})

test_that("NaN is a value and a NULL is NA, through a stream", {
  skip_if_not_installed("zuarrow")
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  DBI::dbWriteTableArrow(con, "t", data.frame(x = c(NaN, 1, -0, NA)))
  got <- DBI::dbGetQuery(con, "SELECT x FROM t")$x
  expect_identical(got, c(NaN, 1, -0, NA))
  expect_identical(1 / got[[3]], -Inf)
  expect_identical(
    DBI::dbGetQuery(con, "SELECT count(*) AS n FROM t WHERE x IS NULL")$n,
    1
  )
})

test_that("batches across a chunk and a row group read back identical", {
  skip_if_not_installed("zuarrow")
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
    DBI::dbWriteTableArrow(con, "t", zua_stream(value, batch = 200000L), overwrite = TRUE)
    expect_identical(
      DBI::dbGetQuery(con, "SELECT * FROM t ORDER BY k"),
      value,
      label = n
    )
  }
  # Batches whose length is not the chunk's, so chunks do not line up with
  # them: 2047, 2047, 2047 and 4 rows.
  value <- frame(6145L)
  DBI::dbWriteTableArrow(con, "u", zua_stream(value, batch = 2047L))
  expect_identical(DBI::dbGetQuery(con, "SELECT * FROM u ORDER BY k"), value)
})

test_that("a column of a type the engine cannot hold is refused from the schema", {
  skip_if_not_installed("zuarrow")
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  # Each column of one row, with the format it is written as where its R
  # class does not say.
  refused <- list(
    dec = list(structure("1.50", arrow_type = "d:10,2")),
    clock = list(structure("01:02:03.000000", arrow_type = "ttu")),
    items = list(list(1:2)),
    rec = list(data.frame(a = 1L)),
    dict = list(factor("a")),
    span = list(as.difftime(1, units = "secs")),
    pairs = list(structure(
      list(data.frame(key = "a", value = 1L)),
      arrow_keys_sorted = FALSE
    )),
    extended = list(structure(1L, arrow_extension = "an.extension")),
    half = list(0.5, "e"),
    months = list(structure("14M", arrow_type = "tiM")),
    nothing = list(NA, "n")
  )
  DBI::dbExecute(con, "CREATE TABLE have (ok INTEGER)")
  for (col in names(refused)) {
    df <- frame_of(ok = 1L, x = refused[[col]][[1L]])
    names(df)[[2L]] <- col
    schema <- if (length(refused[[col]]) > 1L) {
      stats::setNames(refused[[col]][[2L]], col)
    }
    err <- expect_error(
      DBI::dbWriteTableArrow(con, "t", zua_stream(df, schema = schema)),
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
      DBI::dbCreateTableArrow(
        con,
        "t",
        zuarrow::arrow_schema(zua_stream(df, schema = schema))
      ),
      class = "zug_type_error",
      label = col
    )
    expect_false(DBI::dbExistsTable(con, "t"), label = col)
    # An append to a table with that column is refused before any row.
    DBI::dbExecute(con, sprintf('ALTER TABLE have ADD COLUMN "%s" DOUBLE', col))
    err <- expect_error(
      DBI::dbAppendTableArrow(con, "have", zua_stream(df, schema = schema)),
      class = "zug_type_error",
      label = col
    )
    expect_match(
      conditionMessage(err),
      sprintf("Column `%s`", col),
      fixed = TRUE
    )
  }
  expect_identical(DBI::dbGetQuery(con, "SELECT count(*) AS n FROM have")$n, 0)
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
  skip_if_not_installed("zuarrow")
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
  skip_if_not_installed("zuarrow")
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
  expect_error(
    DBI::dbWriteTableArrow(con, "u", list(a = 1L)),
    class = "zug_argument_error"
  )
  expect_false(DBI::dbExistsTable(con, "u"))
})

test_that("dbCreateTableArrow() makes an empty table of the schema's types", {
  skip_if_not_installed("zuarrow")
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  types <- c(
    b = "b", i8 = "c", i16 = "s", i32 = "i", i64 = "l",
    u8 = "C", u16 = "S", u32 = "I", u64 = "L",
    f = "f", g = "g",
    s = "u", ls = "U", sv = "vu",
    z = "z", lz = "Z", zv = "vz", w = "w:4",
    d32 = "tdD", d64 = "tdm",
    tss = "tss:", tsm = "tsm:UTC", tsu = "tsu:Europe/Lisbon", tsn = "tsn:"
  )
  DBI::dbCreateTableArrow(con, "t", zuarrow::arrow_schema(types))
  got <- DBI::dbGetQuery(con, "DESCRIBE t")
  expect_identical(got$column_name, names(types))
  expect_identical(
    got$column_type,
    c(
      "BOOLEAN", "TINYINT", "SMALLINT", "INTEGER", "BIGINT",
      "SMALLINT", "INTEGER", "BIGINT", "BIGINT",
      "FLOAT", "DOUBLE",
      "VARCHAR", "VARCHAR", "VARCHAR",
      "BLOB", "BLOB", "BLOB", "BLOB",
      "DATE", "DATE",
      "TIMESTAMP", "TIMESTAMP", "TIMESTAMP", "TIMESTAMP"
    )
  )
  expect_identical(DBI::dbGetQuery(con, "SELECT count(*) AS n FROM t")$n, 0)
  # A data frame's schema, as zuarrow reads it; an existing table.
  DBI::dbCreateTableArrow(con, "u", data.frame(x = 1L, y = "a"))
  expect_identical(DBI::dbListFields(con, "u"), c("x", "y"))
  expect_error(
    DBI::dbCreateTableArrow(con, "u", data.frame(x = 1L)),
    class = "zug_argument_error"
  )
})

test_that("strings and binaries of every layout read back as written", {
  skip_if_not_installed("zuarrow")
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  s <- c("short", "a string longer than twelve bytes", NA, "", "é中")
  bins <- lapply(s, function(x) if (is.na(x)) NULL else charToRaw(enc2utf8(x)))
  value <- frame_of(s = s, ls = s, sv = s, z = bins, lz = bins, zv = bins)
  schema <- c(s = "u", ls = "U", sv = "vu", z = "z", lz = "Z", zv = "vz")
  DBI::dbWriteTableArrow(con, "t", zua_stream(value, schema = schema))
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
  fixed <- structure(list(as.raw(1:3), as.raw(4:6)), arrow_width = 3L)
  DBI::dbWriteTableArrow(con, "w", frame_of(w = fixed))
  expect_identical(
    unclass(DBI::dbReadTable(con, "w")$w),
    list(as.raw(1:3), as.raw(4:6)),
    ignore_attr = TRUE
  )
})

test_that("a string that is not UTF-8, or holds a NUL, is a type error naming its row", {
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  # No producer makes these: zuarrow refuses to. The package's raw one does.
  for (bytes in list(
    as.raw(c(0x61, 0xff)),
    as.raw(c(0x61, 0x00)),
    as.raw(c(0xc0, 0x80)),
    as.raw(c(0xed, 0xa0, 0x80))
  )) {
    stream <- zug_raw_stream(
      list(s = bytes_column("u", list(charToRaw("ok"), bytes))),
      length = 2
    )
    err <- expect_error(
      DBI::dbWriteTableArrow(con, "t", stream),
      class = "zug_type_error"
    )
    expect_match(conditionMessage(err), "Column `s`, row 2", fixed = TRUE)
    expect_false(DBI::dbExistsTable(con, "t"))
  }
  # The same bytes in a binary column are a value.
  stream <- zug_raw_stream(
    list(b = bytes_column("z", list(as.raw(c(0x61, 0x00, 0xff))))),
    length = 1
  )
  DBI::dbWriteTableArrow(con, "b", stream)
  expect_identical(
    DBI::dbReadTable(con, "b")$b[[1]],
    as.raw(c(0x61, 0x00, 0xff))
  )
})

test_that("unsigned integers widen, and a uint64 past 2^63 - 1 is refused", {
  skip_if_not_installed("zuarrow")
  con <- DBI::dbConnect(zugodb(), bigint = "character")
  withr::defer(DBI::dbDisconnect(con))
  typed <- function(x, format) structure(x, arrow_type = format)
  value <- frame_of(
    u8 = typed(c("0", "255"), "C"),
    u16 = typed(c("0", "65535"), "S"),
    u32 = typed(c("0", "4294967295"), "I"),
    u64 = typed(c("9223372036854775806", "9223372036854775807"), "L")
  )
  DBI::dbWriteTableArrow(con, "t", value)
  got <- DBI::dbReadTable(con, "t")
  expect_identical(got$u8, c(0L, 255L))
  expect_identical(got$u16, c(0L, 65535L))
  expect_identical(got$u32, c("0", "4294967295"))
  expect_identical(got$u64, c("9223372036854775806", "9223372036854775807"))
  big <- frame_of(u64 = typed(c("1", "9223372036854775808"), "L"))
  err <- expect_error(
    DBI::dbWriteTableArrow(con, "u", big),
    class = "zug_type_error"
  )
  expect_match(conditionMessage(err), "Column `u64`, row 2", fixed = TRUE)
  expect_false(DBI::dbExistsTable(con, "u"))
})

test_that("an append straddles each integer column's range and 2^53 for a double", {
  skip_if_not_installed("zuarrow")
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  DBI::dbExecute(
    con,
    "CREATE TABLE t (t8 TINYINT, t16 SMALLINT, t32 INTEGER, d DOUBLE, f FLOAT)"
  )
  # int64 values, by their exact text.
  one <- function(col, x) {
    df <- frame_of(x = structure(sprintf("%.0f", x), arrow_type = "l"))
    names(df) <- col
    df
  }
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
      DBI::dbAppendTableArrow(con, "t", one(col, fits[[col]])),
      2,
      label = col
    )
    for (x in misfits[[col]]) {
      expect_error(
        DBI::dbAppendTableArrow(con, "t", one(col, x)),
        class = "zug_type_error",
        label = paste(col, x)
      )
    }
  }
  expect_error(
    DBI::dbAppendTableArrow(con, "t", data.frame(f = 1e39)),
    class = "zug_type_error"
  )
  expect_identical(DBI::dbAppendTableArrow(con, "t", data.frame(f = 3e38)), 1)
  expect_identical(DBI::dbGetQuery(con, "SELECT count(*) AS n FROM t")$n, 9)
})

test_that("timestamps of each unit and dates of each width are the same instant", {
  skip_if_not_installed("zuarrow")
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  utc <- function(x) as.POSIXct(x, origin = "1970-01-01", tz = "UTC")
  value <- frame_of(
    s = utc(c(-1, 1e9)),
    ms = utc(c(-0.001, 1e9 + 0.25)),
    us = utc(c(-1e-6, 1e9 + 0.5)),
    ns = utc(c(-1e-6, 1e9 + 0.75))
  )
  schema <- c(s = "tss:UTC", ms = "tsm:", us = "tsu:America/New_York", ns = "tsn:")
  DBI::dbWriteTableArrow(con, "t", zua_stream(value, schema = schema))
  got <- DBI::dbReadTable(con, "t")
  expect_identical(got$s, utc(c(-1, 1e9)))
  expect_identical(got$ms, utc(c(-0.001, 1e9 + 0.25)))
  expect_equal(got$us, utc(c(-1e-6, 1e9 + 0.5)), tolerance = 0)
  expect_equal(got$ns, utc(c(-1e-6, 1e9 + 0.75)), tolerance = 0)
  # A fraction of a microsecond is refused, not rounded: 999, 1000, 1001 ns.
  ns <- function(text) {
    frame_of(t = structure(text, arrow_type = "tsn:UTC"))
  }
  expect_error(
    DBI::dbWriteTableArrow(con, "n", ns("1970-01-01T00:00:00.000000999Z")),
    class = "zug_type_error"
  )
  DBI::dbWriteTableArrow(con, "n", ns("1970-01-01T00:00:00.000001000Z"))
  expect_identical(DBI::dbReadTable(con, "n")$t, utc(1e-6))
  expect_error(
    DBI::dbWriteTableArrow(con, "n2", ns("1970-01-01T00:00:00.000001001Z")),
    class = "zug_type_error"
  )
  # Seconds past TIMESTAMP's range overflow when scaled to microseconds.
  expect_error(
    DBI::dbWriteTableArrow(
      con,
      "n3",
      zua_stream(frame_of(t = utc(9223372036855)), schema = c(t = "tss:UTC"))
    ),
    class = "zug_type_error"
  )
  dates <- as.Date(c("1969-12-31", "2024-02-29"))
  DBI::dbWriteTableArrow(
    con,
    "d",
    zua_stream(frame_of(d32 = dates, d64 = dates), schema = c(d64 = "tdm"))
  )
  got <- DBI::dbReadTable(con, "d")
  expect_identical(got$d32, dates)
  expect_identical(got$d64, dates)
  # A date64 that is not a whole day breaks the format's rule, so only the
  # package's raw producer makes one: 86399999, 86400000, 86400001 ms.
  day64 <- function(ms) {
    zug_raw_stream(list(d = fixed_column("tdm", int64_bytes(ms), 1)), length = 1)
  }
  expect_error(
    DBI::dbWriteTableArrow(con, "d2", day64(86399999)),
    class = "zug_type_error"
  )
  DBI::dbWriteTableArrow(con, "d3", day64(86400000))
  expect_identical(DBI::dbReadTable(con, "d3")$d, as.Date("1970-01-02"))
  expect_error(
    DBI::dbWriteTableArrow(con, "d4", day64(86400001)),
    class = "zug_type_error"
  )
})

test_that("a batch with a null row is a stream error", {
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  # Rows 1 and 3 valid, row 2 null, in the batch's own validity.
  stream <- zug_raw_stream(
    list(x = fixed_column("i", int32_bytes(1:3), 3)),
    length = 3,
    null_count = 1,
    validity = as.raw(0x05)
  )
  expect_error(
    DBI::dbWriteTableArrow(con, "t", stream),
    class = "zug_stream_error"
  )
  expect_false(DBI::dbExistsTable(con, "t"))
  # The same batch with every row valid loads.
  stream <- zug_raw_stream(
    list(x = fixed_column("i", int32_bytes(1:3), 3)),
    length = 3,
    validity = as.raw(0x07)
  )
  DBI::dbWriteTableArrow(con, "t", stream)
  expect_identical(DBI::dbReadTable(con, "t")$x, 1:3)
})
