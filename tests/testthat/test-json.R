# The JSON reader (#39): what it reads, it reads as written; what its parsers
# would misread is refused, naming the record and the key (the fork's strict
# validation, d5c7e62). The oracle is the values written into each file
# (json_rows() in helper-results.R).

test_that("flat records read as written, with nulls and missing keys", {
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  got <- json_rows(con, c(
    '{"i": 1, "x": 1.5, "s": "a", "b": true, "n": "z"}',
    '{"i": -2147483647, "x": 2, "s": null, "b": false}',
    '{"x": -1e-3, "s": "", "i": null, "b": null, "n": "y"}'
  ))
  expect_identical(got$i, c(1L, -2147483647L, NA))
  expect_identical(got$x, c(1.5, 2, -1e-3))
  expect_identical(got$s, c("a", NA, ""))
  expect_identical(got$b, c(TRUE, FALSE, NA))
  expect_identical(got$n, c("z", NA, "y"))
})

test_that("one array of objects reads as written", {
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  got <- json_rows(con, '[{"a": 1, "b": "x"}, {"a": 2, "b": "y"}]')
  expect_identical(got[order(names(got))], data.frame(a = 1:2, b = c("x", "y")))
})

test_that("escapes decode, \\u escapes and surrogate pairs included", {
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  got <- json_rows(con, c(
    '{"s": "\\u00e9\\ud83d\\ude00"}',
    '{"s": "a\\"b\\\\c\\/d\\ne\\tf"}',
    '{"s": "\\u0041\\u00DF"}'
  ))$s
  expect_identical(got, c("é\U0001F600", "a\"b\\c/d\ne\tf", "Aß"))
  expect_true(all(Encoding(got[c(1, 3)]) == "UTF-8"))
})

test_that("more records than an engine chunk read as written", {
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con), priority = "first")
  k <- 0:4999
  lines <- sprintf('{"k": %d, "v": "r%d", "x": %s}', k, k,
                   ifelse(k %% 997L == 5L, "null", sprintf("%d.5", k)))
  got <- json_rows(con, lines, ".ndjson")
  got <- got[order(got$k), ]
  expect_identical(got$k, k)
  expect_identical(got$v, sprintf("r%d", k))
  expect_identical(got$x, ifelse(k %% 997L == 5L, NA_real_, k + 0.5))
})

test_that("what the parsers would misread is refused, naming record and key", {
  con <- DBI::dbConnect(zugodb())
  withr::defer(DBI::dbDisconnect(con))
  refused <- list(
    nested_array = list(c('{"a": [1, 2], "b": 1}'), "record 1", "nested array"),
    nested_object = list(c('{"x": 1, "o": {"p": 1}}'), "record 1", "nested object"),
    nested_in_array = list('[{"a": 1}, {"a": [1]}]', "record 2", "nested array"),
    later_key = list(c('{"a": 1}', '{"a": 2, "b": "late"}'), "record 2", "'b' is not in the first record"),
    string_after_int = list(c('{"a": 1}', '{"a": "two"}'), "record 2", "holds a string"),
    float_after_int = list(c('{"a": 1}', '{"a": 3.5}'), "record 2", "fraction"),
    int_after_string = list(c('{"a": "x"}', '{"a": 1}'), "record 2", "holds an integer"),
    number_after_null = list(c('{"a": null}', '{"a": 1}'), "record 2", "first value is null"),
    int_beyond_int32 = list(c('{"a": 1}', '{"a": 2147483648}'), "record 2", "32-bit"),
    int_beyond_int64 = list(c('{"a": 99999999999999999999}'), "record 1", "64 bits"),
    duplicate_key = list(c('{"a": 1, "a": 2}'), "record 1", "appears twice"),
    trailing_comma = list(c('{"a": 1,}'), "record 1", "expected a key"),
    bad_escape = list(c('{"a": "\\x41"}'), "record 1", "bad escape"),
    # writeLines() ends the line, so the scan meets the newline first.
    unterminated = list(c('{"a": "abc'), "record 1", "control character"),
    scalar_record = list(c('42'), "record 1", "not an object")
  )
  for (name in names(refused)) {
    case <- refused[[name]]
    err <- expect_error(
      json_rows(con, case[[1]]), class = "zug_sql_error", label = name
    )
    expect_match(conditionMessage(err), case[[2]], fixed = TRUE, label = name)
    expect_match(conditionMessage(err), case[[3]], fixed = TRUE, label = name)
  }
  # The connection is still good, and so is an empty file.
  expect_identical(json_rows(con, '{"a": 7}')$a, 7L)
})
