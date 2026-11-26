# tools/golden.R: writes the golden database file for test-persist.R (design
# §8, Q7) from the package root, as `Rscript tools/golden.R`, with the
# installed zugodb.
#
#   tests/testthat/fixtures/golden/v1001.zugodb   the database file
#   tests/testthat/fixtures/golden/v1001.rows.R   each table's rows
#
# The same statements run twice: into the file, and in an in-memory
# database whose answers are recorded as the rows. So the test compares
# a file read back against answers no file was involved in, and a change
# to the file's layout made without a new format version fails there,
# not in a user's data. Re-run it only with a new format version, and
# keep the old file as a fixture of its own.

statements <- c(
  "CREATE TABLE ints (k INTEGER, i INTEGER, s SMALLINT, t TINYINT, b BOOLEAN)",
  "INSERT INTO ints VALUES (1, 2147483647, 32767, 127, TRUE),
     (2, -2147483647, -32768, -128, FALSE), (3, 0, 0, 0, NULL), (4, NULL, NULL, NULL, TRUE)",
  "CREATE TABLE big (k INTEGER, v BIGINT)",
  "INSERT INTO big VALUES (1, 9007199254740992), (2, -9007199254740992),
     (3, 4294967296), (4, NULL)",
  "CREATE TABLE dbl (k INTEGER, d DOUBLE, f FLOAT)",
  "INSERT INTO dbl VALUES (1, 1.5, 0.25), (2, -0.0, -2.5), (3, 1e300, 3.0e38),
     (4, CAST('Infinity' AS DOUBLE), CAST('-Infinity' AS FLOAT)),
     (5, CAST('nan' AS DOUBLE), NULL), (6, 2.2250738585072014e-308, 1.0e-38), (7, NULL, 1)",
  "CREATE TABLE txt (k INTEGER, s VARCHAR)",
  "INSERT INTO txt VALUES (1, ''), (2, 'abc'), (3, 'été 日本'), (4, NULL),
     (5, repeat('x', 5000))",
  "CREATE TABLE tm (k INTEGER, d DATE, ts TIMESTAMP)",
  "INSERT INTO tm VALUES (1, DATE '1969-07-20', TIMESTAMP '1969-12-31 23:59:59.5'),
     (2, DATE '2024-02-29', TIMESTAMP '2024-02-29 12:00:00.000001'),
     (3, DATE '0001-01-01', TIMESTAMP '9999-12-31 23:59:59'), (4, NULL, NULL)",
  "CREATE TABLE bl (k INTEGER, b BLOB)",
  "INSERT INTO bl VALUES (1, '\\xAA\\x00b'::BLOB), (2, ''::BLOB), (3, NULL)",
  "CREATE TABLE \"with space\" (\"a b\" INTEGER)",
  "INSERT INTO \"with space\" VALUES (1)",
  "CREATE TABLE empty (k INTEGER, s VARCHAR)"
)

# More rows than an engine vector holds (2048), with NULLs past the first,
# written by dbWriteTable(). Its rows are not recorded: test-persist.R
# builds the same data frame (golden_many()).
many <- local({
  k <- 0:4999
  data.frame(k = k, v = ifelse(k %% 7L == 0L, NA, k * 3), s = paste0("r", k))
})

write_all <- function(con) {
  for (s in statements) DBI::dbExecute(con, s)
  DBI::dbWriteTable(con, "many", many)
}

tables <- c("ints", "big", "dbl", "txt", "tm", "bl", "with space", "many", "empty")

read_all <- function(con) {
  out <- lapply(tables, function(t) {
    DBI::dbGetQuery(con, sprintf("SELECT * FROM \"%s\" ORDER BY 1", t))
  })
  names(out) <- tables
  out
}

main <- function() {
  dir <- file.path("tests", "testthat", "fixtures", "golden")
  dir.create(dir, recursive = TRUE, showWarnings = FALSE)
  path <- file.path(dir, "v1001.zugodb")
  unlink(path)

  mem <- DBI::dbConnect(zugodb::zugodb(), bigint = "character")
  write_all(mem)
  rows <- read_all(mem)
  stopifnot(identical(rows$many, many))
  rows$many <- NULL
  DBI::dbDisconnect(mem, shutdown = TRUE)

  con <- DBI::dbConnect(zugodb::zugodb(), dbdir = path, bigint = "character")
  write_all(con)
  DBI::dbDisconnect(con, shutdown = TRUE)

  info <- zugodb::zug_info()
  text <- c(
    sprintf(
      "# By tools/golden.R with zugodb %s, libzugodb %s (%s): format 1001.",
      utils::packageVersion("zugodb"), info$tag, substr(info$commit, 1, 7)
    ),
    deparse(rows, control = c("keepNA", "keepInteger", "niceNames", "showAttributes", "hexNumeric"))
  )
  writeLines(text, file.path(dir, "v1001.rows.R"), useBytes = TRUE)
  message("wrote ", path, " (", file.size(path), " bytes)")
}

main()
