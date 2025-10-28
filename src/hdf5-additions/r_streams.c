#include "r_streams.h"

#include <stdio.h>       // For FILE, vfprintf, fputs
#include <stdarg.h>      // For va_list, va_start, va_end
#include <R_ext/Print.h> // For Rvprintf, REvprintf, Rprintf, REprintf

/*
 * --- Sentinel Definitions ---
 *
 * We define two static, empty FILE structs. Their only purpose
 * is to provide a unique memory address.
 */
static FILE r_stdout_sentinel;
static FILE r_stderr_sentinel;

/*
 * --- Global Sentinel Pointers ---
 *
 * These are the global variables that the HDF5 codebase will be
 * patched to use. They are exported (not static) so that all
 * compiled HDF5 object files can link to them.
 *
 * In HDF5 code:
 * - 'stdout' will be replaced with 'Rstdout'
 * - 'stderr' will be replaced with 'Rstderr'
 */
FILE *Rstdout = &r_stdout_sentinel;
FILE *Rstderr = &r_stderr_sentinel;


/**
 * @brief Interceptor for fprintf.
 *
 * Replaces fprintf. Checks if the stream is one of our
 * sentinel values. If so, redirects to R's safe printing.
 * Otherwise, passes through to standard vfprintf.
 *
 * In HDF5 code:
 * - 'fprintf' will be replaced with 'Rfprintf'
 */
int Rfprintf(FILE *stream, const char *format, ...)
{
    int ret = 0;
    va_list ap;
    va_start(ap, format);

    if (stream == Rstdout) {
        /* Redirect to R's vprintf (variadic Rprintf) */
        Rvprintf(format, ap);
    }
    else if (stream == Rstderr) {
        /* Redirect to R's vEprintf (variadic REprintf) */
        REvprintf(format, ap);
    }
    else {
        /* This is a real file handle, pass to standard vfprintf */
        ret = vfprintf(stream, format, ap);
    }

    va_end(ap);
    return ret;
}

/**
 * @brief Interceptor for fputs.
 *
 * Replaces fputs. Checks if the stream is one of our
 * sentinel values. If so, redirects to R's safe printing.
 * Otherwise, passes through to standard fputs.
 *
 * In HDF5 code:
 * - 'fputs' will be replaced with 'Rfputs'
 */
int Rfputs(const char *s, FILE *stream)
{
    /* fputs returns non-negative on success */
    int ret = 0; 

    if (stream == Rstdout) {
        /* Rprintf handles the string format */
        Rprintf("%s", s);
    }
    else if (stream == Rstderr) {
        /* REprintf handles the string format */
        REprintf("%s", s);
    }
    else {
        /* This is a real file handle, pass to standard fputs */
        ret = fputs(s, stream);
    }

    return ret;
}

/**
 * @brief Interceptor for abort.
 *
 * Replaces abort(). Calls R's error function instead.
 * Note: 'abort' is a 'noreturn' function. Rf_error() also
 * does not return (it longjumps), so this is a safe replacement.
 */
void Rabort(void)
{
    Rf_error("HDF5 library called abort()");
}

/**
 * @brief Interceptor for exit.
 *
 * Replaces exit(). Calls R's error function instead.
 * Note: 'exit' is a 'noreturn' function. Rf_error() also
 * does not return (it longjumps), so this is a safe replacement.
 */
void Rexit(int status)
{
    Rf_error("HDF5 library called exit() with status %d", status);
}