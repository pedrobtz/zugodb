# Installs DBI and duckdb as binaries, never from source, into the first
# library path, for tools/ci/coexist.R. A source build of duckdb takes 30
# minutes or more, so a missing binary must fail here, not compile.
#
# Binaries: CRAN's own on macOS and Windows; on Linux, where CRAN has none,
# Posit's CRAN-like repository, which builds them per distro
# (__linux__/<codename>, served to a user agent that names R; duckdb 2.0.0
# for noble and resolute, checked 2026-11-17).
#
# No repository here is binary-only: a missing binary falls back to the
# source package. The guard is MAKE=false: installing a binary never runs
# make, so a fallback to source fails at once.
#
# A file, not `Rscript -e`, because a multi-line `-e` dies on Windows before
# it runs anything.
if (!requireNamespace("duckdb", quietly = TRUE)) {
  options(install.packages.compile.from.source = "never")
  Sys.setenv(MAKE = "false")
  posit <- "https://packagemanager.posit.co/cran"
  if (Sys.info()[["sysname"]] == "Linux") {
    os <- readLines("/etc/os-release")
    codename <- sub(".*=", "", grep("^VERSION_CODENAME=", os, value = TRUE))
    repos <- c(P3M = sprintf("%s/__linux__/%s/latest", posit, codename))
    options(HTTPUserAgent = sprintf(
      "R/%s R (%s)", getRversion(),
      paste(getRversion(), R.version$platform, R.version$arch, R.version$os)
    ))
    type <- "source" # Posit serves Linux binaries under src/contrib
  } else {
    repos <- c(CRAN = "https://cloud.r-project.org")
    type <- "binary"
  }
  cat("installing duckdb from", repos, "\n")
  install.packages(
    c("DBI", "duckdb"),
    lib = .libPaths()[1], repos = repos, type = type
  )
  if (!requireNamespace("duckdb", quietly = TRUE)) {
    stop("no duckdb binary was installed (a source build is refused)")
  }
}
cat("duckdb", as.character(utils::packageVersion("duckdb")), "\n")
