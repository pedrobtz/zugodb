# Helpers for test-parquet-stop.R (#32).

# The child: three queries that stop before the file's last row group (a
# bare LIMIT, a filtered LIMIT, a LIMIT past a row group), in turn, each
# answer checked against the file's x = 0, 1, ..., 40959. Alternating them
# is what reached the race; each alone did not in 20 000 runs. More decode
# workers than cores (threads = 6, the scan's own maximum) make a worker
# more likely to be preempted inside the race window.
parquet_stop_child <- function() {
  c(
    'args <- commandArgs(trailingOnly = TRUE)',
    'library(zuslothdb, lib.loc = args[[1]])',
    'f <- gsub("\'", "\'\'", args[[2]], fixed = TRUE)',
    'seconds <- as.numeric(args[[3]])',
    'threads <- as.integer(args[[4]])',
    'q <- sprintf(c(',
    '  "SELECT x FROM \'%s\' LIMIT 1",',
    '  "SELECT x FROM read_parquet(\'%s\') WHERE x %% 7 = 0 LIMIT 3",',
    '  "SELECT x FROM read_parquet(\'%s\') LIMIT 5000"',
    '), f)',
    'con <- DBI::dbConnect(zuslothdb(), threads = threads)',
    'check <- function(d, n, ok) {',
    '  if (!identical(names(d), "x") || nrow(d) != n || !is.integer(d$x) ||',
    '      anyNA(d$x) || !all(ok(d$x))) stop("wrong rows")',
    '}',
    'n <- 0L',
    'end <- Sys.time() + seconds',
    'while (Sys.time() < end) {',
    '  check(DBI::dbGetQuery(con, q[[1]]), 1L, function(x) x >= 0L & x < 40960L)',
    '  check(DBI::dbGetQuery(con, q[[2]]), 3L, function(x) x %% 7L == 0L & x < 40960L)',
    '  d <- DBI::dbGetQuery(con, q[[3]])',
    '  check(d, 5000L, function(x) x >= 0L & x < 40960L)',
    '  if (anyDuplicated(d$x)) stop("wrong rows")',
    '  n <- n + 1L',
    '}',
    'DBI::dbDisconnect(con)',
    'cat("ok", n, "iterations\\n")'
  )
}
