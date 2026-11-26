# Vendor libzugodb, the engine, into src/vendor/libzugodb from tools/vendor.yaml.
#
#   Rscript tools/vendor.R                    # re-vendor and regenerate
#   Rscript tools/vendor.R --verify           # rebuild in a temp dir, diff
#   Rscript tools/vendor.R --source <dir>     # use a local clone of libzugodb
#
# Run from the package root. Needs git and the yaml package. The vendored
# tree is exactly libzugodb at `source.tag`, which must be `source.commit`
# and descend from SlothDB's `origin.commit`: `copy` minus `exclude`. From
# it, this script writes
# src/Makevars, src/Makevars.win, src/zug_provenance.h, inst/vendor.yaml
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
    "libzugodb %s (%s), from SlothDB %s",
    cfg$source$tag, short(cfg$source$commit), short(cfg$origin$commit)
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

# A checkout of libzugodb at `source.tag`, in a temporary directory: a fresh
# clone, or a clone of the local `source_dir`. The tag must be
# `source.commit`, and SlothDB's `origin.commit` an ancestor of it.
patched_checkout <- function(cfg, source_dir) {
  dir <- tempfile("libzugodb-")
  from <- if (is.null(source_dir)) cfg$source$repository else normalizePath(source_dir)
  git("clone", "--quiet", "--no-checkout", from, dir)
  tagged <- git("-C", dir, "rev-parse", paste0(cfg$source$tag, "^{commit}"))
  if (!identical(tagged, cfg$source$commit)) {
    stop("tag ", cfg$source$tag, " is ", tagged, ", not ", cfg$source$commit,
         call. = FALSE)
  }
  git("-C", dir, "-c", "core.autocrlf=false", "-c", "advice.detachedHead=false",
      "checkout", "--quiet", cfg$source$commit)
  base <- system2(
    "git", c("-C", dir, "merge-base", "--is-ancestor", cfg$origin$commit,
             cfg$source$commit)
  )
  if (base != 0) {
    stop("SlothDB ", cfg$origin$commit, " is not an ancestor of ",
         cfg$source$commit, call. = FALSE)
  }
  dir
}

# Relative paths of every file `copy` names, minus `exclude`, sorted in the C
# locale. An exclude that matches nothing is an error: it means libzugodb
# moved and the manifest is stale.
vendored_files <- function(cfg, root) {
  files <- unlist(lapply(cfg$copy, function(entry) {
    path <- file.path(root, entry)
    if (dir.exists(path)) {
      file.path(entry, list.files(path, recursive = TRUE, all.files = TRUE))
    } else if (file.exists(path)) {
      entry
    } else {
      stop("copy entry not found in libzugodb: ", entry, call. = FALSE)
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
  write_provenance(cfg, file.path(out, "src/zug_provenance.h"))
  file.copy("tools/vendor.yaml", file.path(out, "inst/vendor.yaml"),
            overwrite = TRUE)
  invisible(files)
}

write_checksums <- function(dest, files, path) {
  sums <- unname(tools::md5sum(file.path(dest, files)))
  writeLines(paste0(sums, "  ", files), path)
}

# The objects to build: the glue (src/zug_*.cpp, in the package) and every
# vendored translation unit.
objects <- function(cfg, files) {
  units <- files[grepl("\\.(c|cc|cpp)$", files)]
  vendored <- sub("\\.(c|cc|cpp)$", ".o",
                  file.path(sub("^src/", "", cfg$destination), units))
  glue <- sort(list.files("src", pattern = "^zug_.*\\.cpp$"), method = "radix")
  c(sub("\\.cpp$", ".o", glue), vendored)
}

write_makevars <- function(cfg, files, out) {
  v <- sub("^src/", "", cfg$destination)
  objs <- objects(cfg, files)
  common <- c(
    "# Generated by tools/vendor.R from tools/vendor.yaml; do not edit.",
    "CXX_STD = CXX20",
    "",
    paste0("ZUG_INCLUDES = -I", v, "/include"),
    "# NDEBUG: the engine's assert()s would link abort (CRAN's compiled-code",
    "# check).",
    "PKG_CPPFLAGS = $(ZUG_INCLUDES) -DNDEBUG",
    "# Only R_init_zugodb is exported (design §4: the engine's symbols must",
    "# not bind to another package's).",
    "PKG_CXXFLAGS = $(CXX_VISIBILITY)",
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
  lines <- c(
    "// Generated by tools/vendor.R from tools/vendor.yaml; do not edit.",
    "#pragma once",
    sprintf("#define ZUG_SOURCE_REPOSITORY %s", c_string(cfg$source$repository)),
    sprintf("#define ZUG_SOURCE_COMMIT %s", c_string(cfg$source$commit)),
    sprintf("#define ZUG_SOURCE_TAG %s", c_string(cfg$source$tag)),
    sprintf("#define ZUG_ORIGIN_REPOSITORY %s", c_string(cfg$origin$repository)),
    sprintf("#define ZUG_ORIGIN_COMMIT %s", c_string(cfg$origin$commit)),
    sprintf("#define ZUG_ORIGIN_DESCRIBE %s", c_string(cfg$origin$describe))
  )
  writeLines(lines, path)
}

# Every generated file and every vendored file, compared byte for byte.
compare <- function(cfg, built, committed) {
  generated <- c("src/Makevars", "src/Makevars.win", "src/zug_provenance.h",
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
    sprintf("not from libzugodb: %s", setdiff(have, want)),
    sprintf("differs: %s", both[!same])
  )
}

main(commandArgs(trailingOnly = TRUE))
