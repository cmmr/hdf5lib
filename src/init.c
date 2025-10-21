#include <R.h>
#include <Rinternals.h>
#include <stdlib.h> // for NULL
#include <R_ext/Rdynload.h>

/*
  Register routines and disable symbol search.
  This is done even though hdf5lib exports no C functions callable by R,
  purely to satisfy R CMD check.
*/
void R_init_hdf5lib(DllInfo *dll)
{
    // Register NULL routines
    R_registerRoutines(dll, NULL, NULL, NULL, NULL);

    // Disable symbol search
    R_useDynamicSymbols(dll, FALSE);
}