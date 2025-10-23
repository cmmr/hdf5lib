#include <R.h>
#include <Rinternals.h>
#include <stdlib.h> // for NULL
#include <R_ext/Rdynload.h>

/* Declare C function signatures */
extern SEXP C_smoke_test(SEXP sexp_filename);

/* Define Callables */
static const R_CallMethodDef CallEntries[] = {
    {"C_smoke_test", (DL_FUNC) &C_smoke_test, 1},
    {NULL, NULL, 0}
};

/*
  Register routines and disable symbol search.
*/
void R_init_hdf5lib(DllInfo *dll)
{
    // Register C routines callable from R
    R_registerRoutines(dll, NULL, CallEntries, NULL, NULL);

    // Disable symbol search
    R_useDynamicSymbols(dll, FALSE);
}
