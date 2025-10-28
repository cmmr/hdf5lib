#ifndef R_STREAMS_H
#define R_STREAMS_H

/* Don't clutter the namespace. */
#define R_NO_REMAP 1

#include <stdio.h>       // For FILE*
#include <R_ext/Print.h> // For Rprintf

/*
 * --- Global Sentinel Pointer Declarations ---
 *
 * Declares the global sentinel variables that the HDF5
 * codebase will be patched to use. The actual definitions
 * are in r_streams.c.
 *
 * Using 'extern' tells the compiler "these variables exist
 * somewhere else; you will find them at link time."
 */
extern FILE *Rstdout;
extern FILE *Rstderr;

/*
 * --- Function Prototypes ---
 *
 * Declares the interceptor functions so that any HDF5
 * C file that calls them knows their signature.
 */

#ifdef __cplusplus
extern "C" {
#endif

int Rfprintf(FILE *stream, const char *format, ...);
int Rfputs(const char *s, FILE *stream);
void Rabort(void);
void Rexit(int status);

#ifdef __cplusplus
}
#endif

#endif /* R_STREAMS_H */