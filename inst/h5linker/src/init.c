
#include <R.h>
#include <Rinternals.h>
#include <stdlib.h> // for NULL
#include <R_ext/Rdynload.h>

extern SEXP C_smoke_test(SEXP);

static const R_CallMethodDef CallEntries[] = {
  {"C_smoke_test", (DL_FUNC) &C_smoke_test, 1},
  {NULL, NULL, 0}
};

void R_init_h5linker(DllInfo *dll) {
  R_registerRoutines(dll, NULL, CallEntries, NULL, NULL);
  R_useDynamicSymbols(dll, FALSE);
}