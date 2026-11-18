#' Information about the bundled engine
#'
#' Reports which engine source the package was built from: a tagged release
#' of libzugodb, the package's engine, which descends from SlothDB.
#'
#' @return A list with elements `engine` (`"libzugodb"`), `repository`,
#'   `commit` and `tag` (libzugodb's repository, the full commit hash and its
#'   release tag), `origin` and `origin_commit` (SlothDB and the commit
#'   libzugodb descends from), `describe` (the last SlothDB release before
#'   it, as `"v0.2.7+"`) and `threads` (the engine's thread cap, an
#'   integer).
#' @export
#' @examples
#' info <- zug_info()
#' info$commit
#' info$tag
zug_info <- function() {
  .Call(zug_c_info)
}
