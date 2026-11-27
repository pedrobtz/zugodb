# DBItest, the DBI specification's own tests (design §10; roadmap Stage 9),
# for test-DBItest.R.

# The binding cases that fail. The rest pass: they test that a bad call is
# an error, which zug_unsupported is.
bind_cases <- paste(
  c(
    "return_value",
    "return_value_statement",
    "too_many",
    "not_enough",
    "multi_row_unequal_length",
    "unnamed_param_named_placeholders",
    "premature_clear",
    "multi_row",
    "multi_row_zero_length",
    "multi_row_statement",
    "repeated",
    "repeated_statement",
    "repeated_untouched",
    "repeated_untouched_statement",
    "integer",
    "numeric",
    "logical",
    "character",
    "character_escape",
    "factor",
    "date",
    "date_integer",
    "timestamp",
    "timestamp_lt",
    "raw",
    "blob"
  ),
  collapse = "|"
)

# The cases zugodb skips, each a DBItest case name (or a pattern of them,
# as DBItest::test_all()'s `skip` reads it) with the reason. Every one is a
# decision the design records; test-DBItest.R holds each to "still fails",
# so a fix that makes one pass removes it from here.
dbitest_skips <- c(
  # Parameter binding: the engine has no prepared statements (§5, Q3).
  stats::setNames(
    "parameters: the engine has no prepared statements (§5, Q3)",
    paste0("(arrow_|stream_|arrow_stream_)?bind_(", bind_cases, ")")
  ),
  "(send_query|get_query|send_statement|execute)_params" = "parameters: the engine has no prepared statements (§5, Q3)",
  "arrow_(send_query|get_query_arrow)_params" = "parameters: the engine has no prepared statements (§5, Q3)",
  # Transactions: dbBegin() is a zug_unsupported (§5, 2026-11-18).
  begin_commit_return_value = "transactions: the engine has none (§5)",
  begin_rollback_return_value = "transactions: the engine has none (§5)",
  begin_begin = "transactions: the engine has none (§5)",
  begin_commit = "transactions: the engine has none (§5)",
  begin_write_commit = "transactions: the engine has none (§5)",
  begin_rollback = "transactions: the engine has none (§5)",
  begin_write_rollback = "transactions: the engine has none (§5)",
  begin_write_disconnect = "transactions: the engine has none (§5)",
  with_transaction_return_value = "transactions: the engine has none (§5)",
  with_transaction_error_nested = "transactions: the engine has none (§5)",
  with_transaction_success = "transactions: the engine has none (§5)",
  with_transaction_failure = "transactions: the engine has none (§5)",
  with_transaction_break = "transactions: the engine has none (§5)",
  with_transaction_side_effects = "transactions: the engine has none (§5)",
  # TIME: every TIME the engine makes is midnight, so a TIME column and a
  # difftime are refused (§6, #43).
  data_type_driver = "dbDataType() refuses difftime: no TIME (§6, #43)",
  data_type_connection = "dbDataType() refuses difftime: no TIME (§6, #43)",
  data_time = "no TIME (§6, #43)",
  data_time_current = "no TIME, and no current_time (§6, #43)",
  # Functions the engine lacks.
  data_date_current = "the engine has no current_date",
  data_date_current_typed = "the engine has no current_date",
  data_timestamp_current = "the engine has no current_timestamp",
  data_timestamp_current_typed = "the engine has no current_timestamp",
  create_table_value_array = "the engine refuses NUMERIC, as DECIMAL (§6)",
  # 64-bit integers: past 2^53, bigint = "numeric" is a zug_type_error, not
  # a rounded double with a warning (§6), and "integer" is not offered.
  connect_bigint_integer = "bigint takes numeric, integer64, character (§6)",
  data_64_bit_numeric_warning = "a BIGINT past 2^53 is an error, not rounded (§6)",
  data_64_bit_lossless = "a BIGINT past 2^53 is an error, not rounded (§6)",
  roundtrip_64_bit_character = "bigint = 'numeric' gives a double, so as.character() has an exponent (§6)",
  append_roundtrip_64_bit_character = "bigint = 'numeric' gives a double, so as.character() has an exponent (§6)",
  # Writing: a factor is written by its labels, with no warning (§5).
  append_roundtrip_factor = "a factor is written by its labels, with no warning (§5)",
  # Arrow results are later work (§2): the read side is DBI's default,
  # through nanoarrow, which warns converting a timestamp past 2^53
  # microseconds to a double.
  arrow_write_table_arrow_roundtrip_timestamp_extended = "Arrow results are DBI's default, through nanoarrow (§2)",
  arrow_append_table_arrow_roundtrip_timestamp_extended = "Arrow results are DBI's default, through nanoarrow (§2)",
  # A factor as an Arrow stream is a dictionary column, refused (§7.3).
  arrow_write_table_arrow_roundtrip_factor = "an Arrow dictionary column is refused (§7.3)",
  arrow_append_table_arrow_roundtrip_factor = "an Arrow dictionary column is refused (§7.3)",
  # The package and its exports.
  package_name = "the package is zugodb, with no leading R (D12)",
  reexport = "DBI's generics are not re-exported, as duckdb's are not (§5.1)"
)

# Skipped cases that pass only after another skipped case has run: each
# reads what the other leaves behind, so they are skipped together.
dbitest_followers <- c(
  begin_write_disconnect_2 = "reads the table begin_write_disconnect_1 writes"
)

# DBItest's timestamp round trips convert through the zone PST8PDT, which
# tzdata 2024b turned into a legacy link that Debian and Ubuntu ship only in
# tzdata-legacy. Where it is missing lubridate warns, and the case fails on
# DBItest's no-warning check whatever the driver did. They are skipped only
# there; macOS and Windows have the zone, and run them.
dbitest_tz_cases <- c(
  roundtrip_timestamp = "no PST8PDT in this system's time zones",
  roundtrip_timestamp_extended = "no PST8PDT in this system's time zones",
  append_roundtrip_timestamp = "no PST8PDT in this system's time zones",
  append_roundtrip_timestamp_extended = "no PST8PDT in this system's time zones",
  arrow_write_table_arrow_roundtrip_timestamp = "no PST8PDT in this system's time zones",
  arrow_append_table_arrow_roundtrip_timestamp = "no PST8PDT in this system's time zones"
)

# Whether lubridate, as DBItest calls it, knows PST8PDT.
has_pst8pdt <- function() {
  known <- TRUE
  withCallingHandlers(
    lubridate::with_tz(Sys.time(), tzone = "PST8PDT"),
    warning = function(w) {
      known <<- FALSE
      invokeRestart("muffleWarning")
    }
  )
  known
}

# DBItest's cases run under gctorture and valgrind (r-actions sets
# R_ACTIONS_WORKFLOW) for neither: they took the gctorture pass past its
# 10-minute budget and valgrind past its hour. ASan and UBSan, with both
# compilers, run every case.
skip_under_memory_tools <- function() {
  workflow <- Sys.getenv("R_ACTIONS_WORKFLOW")
  if (workflow %in% c("gctorture", "valgrind")) {
    testthat::skip(paste("DBItest is left out of", workflow))
  }
}

# The context: zugodb's own spelling of dates, timestamps and placeholders,
# no TIME and no temporary tables (§5, §6), every case of this DBItest.
dbitest_context <- function() {
  DBItest::make_context(
    zugodb(),
    list(),
    tweaks = DBItest::tweaks(
      constructor_relax_args = TRUE,
      date_cast = function(x) paste0("CAST('", x, "' AS DATE)"),
      timestamp_cast = function(x) paste0("CAST('", x, "' AS TIMESTAMP)"),
      time_typed = FALSE,
      temporary_tables = FALSE,
      list_temporary_tables = FALSE,
      # So the bind_ cases run, and fail, rather than skip themselves.
      placeholder_pattern = "?",
      dbitest_version = as.character(utils::packageVersion("DBItest"))
    ),
    name = "zugodb",
    default_skip = character()
  )
}

# DBItest's cases matching `patterns`, run and reported to a ListReporter:
# a data frame of each case's name and whether it failed.
dbitest_outcomes <- function(ctx, patterns) {
  rep <- testthat::ListReporter$new()
  testthat::with_reporter(rep, {
    rep$start_file("dbitest-skipped")
    # As `skip` matches: a pattern, or it with a numbered suffix.
    DBItest::test_all(
      run_only = paste0("(?:", patterns, ")(?:|_[0-9]+)"),
      ctx = ctx
    )
    rep$end_file()
  })
  res <- as.data.frame(rep$get_results())
  data.frame(
    test = sub("^DBItest\\[zugodb\\]: [^:]*: ", "", res$test),
    failed = res$failed > 0 | res$error,
    skipped = res$skipped
  )
}
