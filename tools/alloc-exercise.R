# The workload alloc-failure.yaml sweeps: one allocation fails per run, and
# the run must end in an error the package raised (or R's own "cannot
# allocate"), never a crash. The engine's failed `new` is a std::bad_alloc,
# which the glue catches and returns (design §4.3), so it surfaces as a
# zuslothdb_error whose message names bad_alloc.
#
# The result is checked, so an allocation failure that is swallowed and
# turns into a wrong answer fails the run instead of passing it.
suppressPackageStartupMessages(library(zuslothdb))
con <- DBI::dbConnect(zuslothdb(), threads = 1L)
DBI::dbExecute(con, "CREATE TABLE t (i INTEGER, s VARCHAR)")
DBI::dbExecute(con, "INSERT INTO t VALUES (1, 'héllo'), (2, NULL), (3, '漢字')")
res <- DBI::dbSendQuery(con, "SELECT i, s, CAST(i AS BIGINT) * 1000 AS g FROM t ORDER BY i")
a <- DBI::dbFetch(res, 2)
b <- DBI::dbFetch(res)
DBI::dbClearResult(res)
DBI::dbDisconnect(con)
stopifnot(
  identical(c(a$i, b$i), 1:3),
  identical(c(a$s, b$s), c("héllo", NA, "漢字")),
  identical(c(a$g, b$g), c(1000, 2000, 3000))
)
cat("ok\n")
