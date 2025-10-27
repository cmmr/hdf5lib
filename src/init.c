
#include <R_ext/Rdynload.h>
#include <stdlib.h> // for NULL

/*
 * This init function is for the 'hdf5lib' package itself.
 * By calling R_useDynamicSymbols(dll, TRUE), we tell R that
 * this package (and any library it loads, like libhdf5.so)
 * is allowed to search for R's internal symbols.
*/
void R_init_hdf5lib(DllInfo *dll) {
    R_registerRoutines(dll, NULL, NULL, NULL, NULL);
    R_useDynamicSymbols(dll, TRUE);
}
