# Child R processes, for tests that need a fresh session (test-persist.R).

# Runs `script` in a child R with `args` after --args, as tools:::R_runR()
# starts R: Rterm.exe on Windows, where Rscript exited 1033 without
# running the script under R CMD check, and R elsewhere. Returns its
# output, with a "status" attribute when it did not exit 0 (124: killed at
# `timeout` seconds).
run_child_r <- function(script, args, timeout) {
  windows <- .Platform$OS.type == "windows"
  quoting <- if (windows) "cmd" else "sh"
  r <- file.path(R.home("bin"), if (windows) "Rterm.exe" else "R")
  suppressWarnings(system2(
    r,
    c(
      "--no-echo",
      "--no-restore",
      paste("-f", shQuote(script, quoting)),
      "--args",
      shQuote(args, quoting)
    ),
    stdout = TRUE,
    stderr = TRUE,
    timeout = timeout
  ))
}
