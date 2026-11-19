# Records duckdb's answer to each query in tests/testthat/oracle/ (design
# §10): the tests compare zugodb's result with these answers, so the
# oracle is checked on every CI leg without duckdb being installed.
#
#   Rscript tools/oracle.R             # every *.sql
#   Rscript tools/oracle.R scalar-int  # those whose name matches
#
# A development tool: it needs duckdb, which the package does not suggest.
#
# Each <name>.sql holds statements separated by a line ending in ";". The
# last statement is the query; the ones before it set up its tables. The
# answer goes to <name>.answer.R as R code (dput), headed by the duckdb
# version that gave it: either the data frame, or list(error = <message>)
# when duckdb refuses any of the statements.

main <- function(args) {
  if (!requireNamespace("duckdb", quietly = TRUE)) {
    stop("tools/oracle.R needs duckdb", call. = FALSE)
  }
  dir <- "tests/testthat/oracle"
  files <- sort(list.files(dir, pattern = "\\.sql$", full.names = TRUE))
  if (length(args)) {
    files <- files[grepl(paste(args, collapse = "|"), basename(files))]
  }
  version <- as.character(utils::packageVersion("duckdb"))
  for (f in files) {
    answer <- duckdb_answer(oracle_statements(f))
    out <- sub("\\.sql$", ".answer.R", f)
    text <- c(
      sprintf("# duckdb %s, by tools/oracle.R, for %s", version, basename(f)),
      deparse(answer, control = c("keepNA", "keepInteger", "niceNames", "showAttributes", "hexNumeric"))
    )
    writeLines(text, out, useBytes = TRUE)
    message("recorded ", basename(out))
  }
}

# The statements of a corpus file: split after each line ending in ";".
# Shared with tests/testthat/helper-oracle.R, which has its own copy.
oracle_statements <- function(path) {
  lines <- readLines(path, encoding = "UTF-8", warn = FALSE)
  lines <- lines[!grepl("^\\s*--", lines)]
  text <- paste(lines, collapse = "\n")
  parts <- strsplit(text, ";\\s*(\n|$)")[[1]]
  parts <- trimws(parts)
  parts[nzchar(parts)]
}

duckdb_answer <- function(statements) {
  con <- DBI::dbConnect(duckdb::duckdb(shared_home = FALSE))
  on.exit(DBI::dbDisconnect(con, shutdown = TRUE))
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

main(commandArgs(trailingOnly = TRUE))
