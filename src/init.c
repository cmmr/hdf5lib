// src/init.c
#include <R.h>
#include <Rinternals.h>
#include <stdlib.h> // for NULL
#include <R_ext/Rdynload.h>

// A dummy function to ensure the file isn't completely empty.
void hdf5lib_dummy_init(void) {
    // No operation
}

// Optional, but recommended registration function
void R_init_hdf5lib(DllInfo *dll) {
    // Register routines, call specifications, etc.
    // Since this package only provides a library to link against,
    // all arguments are NULL.
    R_registerRoutines(dll, NULL, NULL, NULL, NULL);
    
    // Recommended practice: prevent dynamic symbol lookup
    R_useDynamicSymbols(dll, FALSE); 
}
