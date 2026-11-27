# Deep SQL expressions for test-hostile.R (libzugodb 0.4.1, #65).

# `first`, then `ops` times " op term": an expression tree of height
# ops + 1 when `first` and `term` are leaves.
sql_chain <- function(first, op, term, ops) {
  paste0(first, strrep(paste0(" ", op, " ", term), ops))
}

# The first column of the query's one row, or the condition it raised.
query_or_condition <- function(con, sql) {
  tryCatch(DBI::dbGetQuery(con, sql)[[1L]], error = function(e) e)
}
