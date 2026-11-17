# The recorded-answer oracle (design §10; tools/oracle.R writes the
# answers). Each tests/testthat/oracle/<name>.sql has its duckdb answer in
# <name>.answer.R; known-differences.md lists those zuslothdb answers
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

# zuslothdb's answer to a corpus file, in the shape the answers have: the
# data frame with compact row names, or list(error = <message>) if any
# statement fails.
zuslothdb_answer <- function(name, bigint = "numeric") {
  statements <- oracle_statements(file.path(oracle_dir(), paste0(name, ".sql")))
  con <- DBI::dbConnect(zuslothdb(), bigint = bigint)
  on.exit(DBI::dbDisconnect(con))
  n <- length(statements)
  tryCatch(
    {
      for (s in statements[-n]) DBI::dbExecute(con, s)
      df <- DBI::dbGetQuery(con, statements[[n]])
      attr(df, "row.names") <- .set_row_names(nrow(df))
      df
    },
    error = function(e) list(error = conditionMessage(e))
  )
}

# The names listed in known-differences.md's table, first column.
oracle_known_differences <- function() {
  lines <- readLines(file.path(oracle_dir(), "known-differences.md"),
                     encoding = "UTF-8")
  rows <- grep("^\\| `[^`]+` \\|", lines, value = TRUE)
  sub("^\\| `([^`]+)` \\|.*$", "\\1", rows)
}

oracle_names <- function(prefix) {
  files <- list.files(oracle_dir(), pattern = paste0("^", prefix, ".*\\.sql$"))
  sub("\\.sql$", "", sort(files))
}
