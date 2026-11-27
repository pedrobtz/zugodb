# The recorded-answer oracle (design §10; tools/oracle.R writes the
# answers). Each tests/testthat/oracle/<name>.sql has its duckdb answer in
# <name>.answer.R; known-differences.md lists those zugodb answers
# differently, and why.

oracle_dir <- function() test_path("oracle")

# The statements of a corpus file: split after each line ending in ";".
# The same rule as tools/oracle.R's oracle_statements().
oracle_statements <- function(path) {
  lines <- readLines(path, encoding = "UTF-8", warn = FALSE)
  lines <- lines[!grepl("^\\s*--", lines)]
  text <- paste(lines, collapse = "\n")
  parts <- strsplit(text, ";\\s*(\n|$)")[[1]]
  parts <- trimws(parts)
  parts[nzchar(parts)]
}

oracle_answer <- function(name) {
  path <- file.path(oracle_dir(), paste0(name, ".answer.R"))
  eval(parse(path, encoding = "UTF-8", keep.source = FALSE)[[1]], baseenv())
}

# zugodb's answer to a corpus file, in the shape the answers have: the
# data frame with compact row names, or list(error = <message>) if any
# statement fails.
zugodb_answer <- function(
  name,
  bigint = "numeric",
  timezone_out = "UTC",
  threads = 2L
) {
  statements <- oracle_statements(file.path(oracle_dir(), paste0(name, ".sql")))
  con <- DBI::dbConnect(
    zugodb(),
    bigint = bigint,
    timezone_out = timezone_out,
    threads = threads
  )
  on.exit(DBI::dbDisconnect(con))
  n <- length(statements)
  tryCatch(
    {
      for (s in statements[-n]) {
        DBI::dbExecute(con, s)
      }
      df <- DBI::dbGetQuery(con, statements[[n]])
      attr(df, "row.names") <- .set_row_names(nrow(df))
      df
    },
    error = function(e) list(error = conditionMessage(e))
  )
}

# The names listed in known-differences.md's tables, first column, of those
# starting with `prefix`.
oracle_known_differences <- function(prefix = "diff-") {
  lines <- readLines(
    file.path(oracle_dir(), "known-differences.md"),
    encoding = "UTF-8"
  )
  rows <- grep("^\\| `[^`]+` \\|", lines, value = TRUE)
  names <- sub("^\\| `([^`]+)` \\|.*$", "\\1", rows)
  names[startsWith(names, prefix)]
}

oracle_names <- function(prefix) {
  files <- list.files(oracle_dir(), pattern = paste0("^", prefix, ".*\\.sql$"))
  sub("\\.sql$", "", sort(files))
}

# The condition zugodb raises on a corpus file, or NULL if every statement
# succeeds; and whether the connection still answers a query after it.
zugodb_condition <- function(name) {
  statements <- oracle_statements(file.path(oracle_dir(), paste0(name, ".sql")))
  con <- DBI::dbConnect(zugodb())
  on.exit(DBI::dbDisconnect(con))
  cnd <- tryCatch(
    {
      for (s in statements[-length(statements)]) {
        DBI::dbExecute(con, s)
      }
      DBI::dbGetQuery(con, statements[[length(statements)]])
      NULL
    },
    error = function(e) e
  )
  list(
    condition = cnd,
    after = DBI::dbGetQuery(con, "SELECT 1 AS one")$one
  )
}
