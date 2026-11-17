// The scaffold's C entry point, kept only until Stage 1 replaces it with
// the C++20 glue: registers no routines and turns off symbol search, so
// R CMD check's compiled-code check is clean meanwhile.
#define R_NO_REMAP
#include <R.h>
#include <Rinternals.h>
#include <R_ext/Rdynload.h>

void R_init_zuslothdb(DllInfo *dll) {
  R_registerRoutines(dll, NULL, NULL, NULL, NULL);
  R_useDynamicSymbols(dll, FALSE);
}
