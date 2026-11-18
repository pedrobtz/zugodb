# Vendor SlothDB into src/vendor/slothdb from tools/vendor.yaml.
#
#   Rscript tools/vendor.R                    # re-vendor and regenerate
#   Rscript tools/vendor.R --verify           # rebuild in a temp dir, diff
#   Rscript tools/vendor.R --source <dir>     # use a local clone of the fork
#
# Run from the package root. Needs git and the yaml package. The vendored
# tree is exactly the fork (pedrobtz/slothdb) at `source.commit`, `copy`
# minus `exclude`; the fork's history since `upstream.commit` must be
# exactly the commits `changes` lists, in order. From it, this script writes
# src/Makevars, src/Makevars.win, src/sdb_provenance.h, inst/vendor.yaml
# and tools/vendor/checksums.md5. --verify writes nothing in the package:
# it builds the same outputs in a temporary directory and fails, naming
# each file, when any differs from what is committed (r-actions vendor.yml
# runs it through tools/vendor/verify).

main <- function(args) {
  verify <- "--verify" %in% args
  upstream_dir <- arg_value(args, "--source")
  if (!requireNamespace("yaml", quietly = TRUE)) {
    stop("tools/vendor.R needs the yaml package", call. = FALSE)
  }
  cfg <- yaml::read_yaml("tools/vendor.yaml")
  out <- if (verify) tempfile("vendor-verify-") else "."
  build(cfg, upstream_dir, out)
  if (verify) {
    bad <- compare(cfg, out, ".")
    if (length(bad)) {
      message("vendored tree differs from tools/vendor.yaml's source:")
      message(paste0("  ", bad, collapse = "\n"))
      quit(status = 1)
    }
    message("ok: the vendored tree is ", describe(cfg))
  } else {
    message("vendored ", describe(cfg), " into ", cfg$destination)
  }
}

arg_value <- function(args, name) {
  i <- match(name, args)
  if (is.na(i)) NULL else args[[i + 1L]]
}

short <- function(x) substr(x, 1, 7)

describe <- function(cfg) {
  sprintf(
    "the fork at %s (%s): upstream %s + %d changes",
    short(cfg$source$commit), cfg$source$tag, short(cfg$upstream$commit),
    length(cfg$changes)
  )
}

git <- function(...) {
  res <- system2("git", c(...), stdout = TRUE, stderr = TRUE)
  status <- attr(res, "status")
  if (!is.null(status) && status != 0) {
    stop("git ", paste(c(...), collapse = " "), " failed:\n",
         paste(res, collapse = "\n"), call. = FALSE)
  }
  res
}

# A checkout of the fork at `source.commit`, in a temporary directory: a
# fresh clone, or a clone of the local `source_dir`. Its history since
# `upstream.commit` must be exactly `changes`: upstream is an ancestor, and
# every change zuslothdb makes to it is listed with its reason.
patched_checkout <- function(cfg, source_dir) {
  dir <- tempfile("slothdb-")
  from <- if (is.null(source_dir)) cfg$source$repository else normalizePath(source_dir)
  git("clone", "--quiet", "--no-checkout", from, dir)
  git("-C", dir, "-c", "core.autocrlf=false", "-c", "advice.detachedHead=false",
      "checkout", "--quiet", cfg$source$commit)
  head <- git("-C", dir, "rev-parse", "HEAD")
  if (!identical(head, cfg$source$commit)) {
    stop("checkout is at ", head, ", not ", cfg$source$commit, call. = FALSE)
  }
  base <- system2(
    "git", c("-C", dir, "merge-base", "--is-ancestor", cfg$upstream$commit, head)
  )
  if (base != 0) {
    stop("upstream ", cfg$upstream$commit, " is not an ancestor of ", head,
         call. = FALSE)
  }
  history <- git("-C", dir, "rev-list", "--reverse",
                 paste0(cfg$upstream$commit, "..", head))
  listed <- vapply(cfg$changes, `[[`, "", "commit")
  if (!identical(history, listed)) {
    stop("the fork's commits since upstream are not tools/vendor.yaml's ",
         "changes:\n  fork:   ", paste(short(history), collapse = " "),
         "\n  listed: ", paste(short(listed), collapse = " "), call. = FALSE)
  }
  subjects <- git("-C", dir, "log", "--reverse", "--format=%s",
                  paste0(cfg$upstream$commit, "..", head))
  wrong <- subjects != vapply(cfg$changes, `[[`, "", "subject")
  if (any(wrong)) {
    stop("subject differs from the fork's for change ",
         short(listed[wrong][1]), call. = FALSE)
  }
  dir
}

# Relative paths of every file `copy` names, minus `exclude`, sorted in the C
# locale. An exclude that matches nothing is an error: it means upstream
# moved and the manifest is stale.
vendored_files <- function(cfg, root) {
  files <- unlist(lapply(cfg$copy, function(entry) {
    path <- file.path(root, entry)
    if (dir.exists(path)) {
      file.path(entry, list.files(path, recursive = TRUE, all.files = TRUE))
    } else if (file.exists(path)) {
      entry
    } else {
      stop("copy entry not found upstream: ", entry, call. = FALSE)
    }
  }))
  for (ex in cfg$exclude) {
    hit <- files == ex$path | startsWith(files, paste0(ex$path, "/"))
    if (!any(hit)) {
      stop("exclude entry matches nothing: ", ex$path, call. = FALSE)
    }
    files <- files[!hit]
  }
  sort(files, method = "radix")
}

build <- function(cfg, upstream_dir, out) {
  src <- patched_checkout(cfg, upstream_dir)
  on.exit(unlink(src, recursive = TRUE), add = TRUE)
  files <- vendored_files(cfg, src)

  dest <- file.path(out, cfg$destination)
  unlink(dest, recursive = TRUE)
  for (d in unique(dirname(file.path(dest, files)))) {
    dir.create(d, recursive = TRUE, showWarnings = FALSE)
  }
  ok <- file.copy(file.path(src, files), file.path(dest, files))
  if (!all(ok)) stop("copy failed: ", files[!ok][1], call. = FALSE)

  for (d in c("src", "inst", "tools/vendor")) {
    dir.create(file.path(out, d), recursive = TRUE, showWarnings = FALSE)
  }
  write_checksums(dest, files, file.path(out, "tools/vendor/checksums.md5"))
  write_makevars(cfg, files, out)
  write_provenance(cfg, file.path(out, "src/sdb_provenance.h"))
  file.copy("tools/vendor.yaml", file.path(out, "inst/vendor.yaml"),
            overwrite = TRUE)
  invisible(files)
}

write_checksums <- function(dest, files, path) {
  sums <- unname(tools::md5sum(file.path(dest, files)))
  writeLines(paste0(sums, "  ", files), path)
}

# The objects to build: the glue (src/sdb_*.cpp, in the package) and every
# vendored translation unit.
objects <- function(cfg, files) {
  units <- files[grepl("\\.(c|cc|cpp)$", files)]
  vendored <- sub("\\.(c|cc|cpp)$", ".o",
                  file.path(sub("^src/", "", cfg$destination), units))
  glue <- sort(list.files("src", pattern = "^sdb_.*\\.cpp$"), method = "radix")
  c(sub("\\.cpp$", ".o", glue), vendored)
}

write_makevars <- function(cfg, files, out) {
  v <- sub("^src/", "", cfg$destination)
  objs <- objects(cfg, files)
  common <- c(
    "# Generated by tools/vendor.R from tools/vendor.yaml; do not edit.",
    "CXX_STD = CXX20",
    "",
    paste0("SDB_INCLUDES = -I", v, "/include -I", v,
           "/third_party/zstd/lib -I", v, "/third_party/fastpforlib -I", v,
           "/third_party/snappy"),
    "# NDEBUG: upstream's assert()s would link abort (CRAN's compiled-code",
    "# check). ZSTD_DISABLE_ASM: the C path, as upstream builds it.",
    "# ZSTD*_VISIBLE empty: zstd marks its API visibility(\"default\"), which",
    "# would export it past $(C_VISIBILITY) and let another package's zstd",
    "# bind to this one.",
    paste("PKG_CPPFLAGS = $(SDB_INCLUDES) -DNDEBUG -DZSTD_DISABLE_ASM=1",
          "-DZSTD_MULTITHREAD=0 -DZSTDLIB_VISIBLE= -DZSTDERRORLIB_VISIBLE="),
    "# Only R_init_zuslothdb is exported (design §4: SlothDB carries DuckDB's",
    "# duckdb_fastpforlib namespace, which would clash with duckdb's).",
    "PKG_CXXFLAGS = $(CXX_VISIBILITY)",
    "PKG_CFLAGS = $(C_VISIBILITY)",
    "PKG_LIBS = -pthread",
    "",
    paste0("OBJECTS = ", paste(objs, collapse = " \\\n  "))
  )
  writeLines(common, file.path(out, "src/Makevars"))
  writeLines(common, file.path(out, "src/Makevars.win"))
}

# A C string literal: the subjects are ASCII, but a quote or backslash
# would end one.
c_string <- function(x) {
  paste0("\"", gsub("([\"\\\\])", "\\\\\\1", x), "\"")
}

write_provenance <- function(cfg, path) {
  changes <- vapply(cfg$changes, `[[`, "", "subject")
  if (any(grepl("[^ -~]", changes))) stop("a change's subject is not ASCII", call. = FALSE)
  lines <- c(
    "// Generated by tools/vendor.R from tools/vendor.yaml; do not edit.",
    "#pragma once",
    sprintf("#define SDB_SOURCE_REPOSITORY %s", c_string(cfg$source$repository)),
    sprintf("#define SDB_SOURCE_COMMIT %s", c_string(cfg$source$commit)),
    sprintf("#define SDB_SOURCE_TAG %s", c_string(cfg$source$tag)),
    sprintf("#define SDB_UPSTREAM_REPOSITORY %s", c_string(cfg$upstream$repository)),
    sprintf("#define SDB_UPSTREAM_COMMIT %s", c_string(cfg$upstream$commit)),
    sprintf("#define SDB_UPSTREAM_DESCRIBE %s", c_string(cfg$upstream$describe)),
    sprintf("#define SDB_CHANGE_COUNT %d", length(changes)),
    sprintf("#define SDB_CHANGES {%s}", paste(c_string(changes), collapse = ", "))
  )
  writeLines(lines, path)
}

# Every generated file and every vendored file, compared byte for byte.
compare <- function(cfg, built, committed) {
  generated <- c("src/Makevars", "src/Makevars.win", "src/sdb_provenance.h",
                 "inst/vendor.yaml", "tools/vendor/checksums.md5")
  rel <- function(root) {
    d <- file.path(root, cfg$destination)
    if (!dir.exists(d)) return(character())
    file.path(cfg$destination, list.files(d, recursive = TRUE, all.files = TRUE))
  }
  want <- rel(built)
  have <- setdiff(rel(committed), file.path(cfg$destination, ".gitignore"))
  have <- have[!grepl("\\.(o|so|dll|dylib)$", have)]
  both <- c(intersect(want, have), generated)
  same <- vapply(both, function(f) {
    a <- file.path(built, f)
    b <- file.path(committed, f)
    file.exists(b) && identical(unname(tools::md5sum(a)), unname(tools::md5sum(b)))
  }, logical(1))
  # sprintf() of a zero-length vector is zero-length; paste() is not.
  c(
    sprintf("missing: %s", setdiff(want, have)),
    sprintf("not from upstream: %s", setdiff(have, want)),
    sprintf("differs: %s", both[!same])
  )
}

main(commandArgs(trailingOnly = TRUE))
