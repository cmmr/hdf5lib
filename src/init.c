#include <R.h>
#include <Rinternals.h>
#include <stdlib.h> // for NULL
#include <R_ext/Rdynload.h>

/* Declare C function signatures */
extern SEXP C_hdf5_version(SEXP sexp_filename);

/* Define Callables */
static const R_CallMethodDef CallEntries[] = {
    {"C_hdf5_version", (DL_FUNC) &C_hdf5_version, 1},
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
