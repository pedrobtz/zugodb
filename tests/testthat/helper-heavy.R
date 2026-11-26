# ZUG_SKIP_HEAVY=true skips the large cases of a test (design §10): they run
# on every CI leg, where it is unset, and can be left out of a quick local
# loop.
skip_if_heavy <- function() {
  if (identical(tolower(Sys.getenv("ZUG_SKIP_HEAVY")), "true")) {
    testthat::skip("ZUG_SKIP_HEAVY is set")
  }
}
