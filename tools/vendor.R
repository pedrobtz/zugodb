# Vendor SlothDB into src/vendor/slothdb from tools/vendor.yaml.
#
#   Rscript tools/vendor.R                    # re-vendor and regenerate
#   Rscript tools/vendor.R --verify           # rebuild in a temp dir, diff
#   Rscript tools/vendor.R --upstream <dir>   # use a local clone, no fetch
#
# Run from the package root. Needs git and the yaml package. The vendored
# tree is exactly: upstream at `upstream.commit`, `patches` applied with
# `git apply`, `copy` minus `exclude`. From it, this script writes
# src/Makevars, src/Makevars.win, src/sdb_provenance.h, inst/vendor.yaml
# and tools/vendor/checksums.md5. --verify writes nothing in the package:
# it builds the same outputs in a temporary directory and fails, naming
# each file, when any differs from what is committed (r-actions vendor.yml
# runs it through tools/vendor/verify).

main <- function(args) {
  verify <- "--verify" %in% args
  upstream_dir <- arg_value(args, "--upstream")
  if (!requireNamespace("yaml", quietly = TRUE)) {
    stop("tools/vendor.R needs the yaml package", call. = FALSE)
  }
  cfg <- yaml::read_yaml("tools/vendor.yaml")
  out <- if (verify) tempfile("vendor-verify-") else "."
  build(cfg, upstream_dir, out)
  if (verify) {
    bad <- compare(cfg, out, ".")
    if (length(bad)) {
      message("vendored tree differs from tools/vendor.yaml + tools/patches:")
      message(paste0("  ", bad, collapse = "\n"))
      quit(status = 1)
    }
    message("ok: the vendored tree is upstream ", short(cfg), " + ",
            length(cfg$patches), " patches")
  } else {
    message("vendored upstream ", short(cfg), " with ",
            length(cfg$patches), " patches into ", cfg$destination)
  }
}

arg_value <- function(args, name) {
  i <- match(name, args)
  if (is.na(i)) NULL else args[[i + 1L]]
}

short <- function(cfg) substr(cfg$upstream$commit, 1, 7)

git <- function(...) {
  res <- system2("git", c(...), stdout = TRUE, stderr = TRUE)
  status <- attr(res, "status")
  if (!is.null(status) && status != 0) {
    stop("git ", paste(c(...), collapse = " "), " failed:\n",
         paste(res, collapse = "\n"), call. = FALSE)
  }
  res
}

# A checkout of upstream at the pinned commit with the patches applied, in a
# temporary directory: a fresh clone, or a copy of `upstream_dir`.
patched_checkout <- function(cfg, upstream_dir) {
  dir <- tempfile("slothdb-")
  if (is.null(upstream_dir)) {
    git("clone", "--quiet", "--no-checkout", cfg$upstream$repository, dir)
  } else {
    git("clone", "--quiet", "--no-checkout", normalizePath(upstream_dir), dir)
  }
  git("-C", dir, "-c", "core.autocrlf=false", "-c", "advice.detachedHead=false",
      "checkout", "--quiet", cfg$upstream$commit)
  head <- git("-C", dir, "rev-parse", "HEAD")
  if (!identical(head, cfg$upstream$commit)) {
    stop("checkout is at ", head, ", not ", cfg$upstream$commit, call. = FALSE)
  }
  for (p in cfg$patches) {
    git("-C", dir, "apply", "--whitespace=nowarn",
        normalizePath(file.path("tools/patches", p$file)))
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

write_provenance <- function(cfg, path) {
  patches <- vapply(cfg$patches, `[[`, "", "file")
  lines <- c(
    "// Generated by tools/vendor.R from tools/vendor.yaml; do not edit.",
    "#pragma once",
    sprintf("#define SDB_UPSTREAM_REPOSITORY \"%s\"", cfg$upstream$repository),
    sprintf("#define SDB_UPSTREAM_COMMIT \"%s\"", cfg$upstream$commit),
    sprintf("#define SDB_UPSTREAM_DESCRIBE \"%s\"", cfg$upstream$describe),
    sprintf("#define SDB_PATCH_COUNT %d", length(patches)),
    sprintf("#define SDB_PATCHES {%s}",
            paste0("\"", patches, "\"", collapse = ", "))
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
