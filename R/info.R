#' Information about the bundled engine
#'
#' Reports which SlothDB source the package was built from and how it was
#' changed: the upstream repository and commit, and the patches applied to
#' it, in order. A database file is tied to this commit.
#'
#' @return A list with elements `engine` (`"SlothDB"`), `repository`,
#'   `commit` (the full upstream commit hash), `describe` (the last upstream
#'   release before it, as `"v0.2.7+"`), `patches` (a character vector of
#'   patch file names, in the order they are applied) and `threads` (the
#'   engine's thread cap, an integer).
#' @export
#' @examples
#' info <- zuslothdb_info()
#' info$commit
#' info$patches
zuslothdb_info <- function() {
  .Call(sdb_info)
}
