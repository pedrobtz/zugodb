#' Information about the bundled engine
#'
#' Reports which SlothDB source the package was built from. The engine is
#' vendored from pedrobtz/slothdb, zuslothdb's fork of upstream SlothDB: an
#' upstream commit and the changes made on top of it, one commit each. A
#' database file is tied to this commit.
#'
#' @return A list with elements `engine` (`"SlothDB"`), `repository` and
#'   `commit` (the fork and its full commit hash), `tag` (the fork's tag of
#'   that commit), `upstream` and `upstream_commit` (upstream SlothDB and
#'   the commit the fork is based on), `describe` (the last upstream release
#'   before it, as `"v0.2.7+"`), `changes` (a character vector of the
#'   fork's commit subjects since that commit, oldest first) and `threads`
#'   (the engine's thread cap, an integer).
#' @export
#' @examples
#' info <- zuslothdb_info()
#' info$commit
#' info$changes
zuslothdb_info <- function() {
  .Call(sdb_info)
}
