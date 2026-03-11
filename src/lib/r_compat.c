#include "r_compat.h"

/*******************/
/* R Compatibility */
/*******************/

/*
 * --- Sentinel Definitions ---
 *
 * We define two static char variables. Their only purpose
 * is to provide a unique memory address.
 */
static char r_stdout_sentinel;
static char r_stderr_sentinel;

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
FILE *Rstdout = (FILE *)&r_stdout_sentinel;
FILE *Rstderr = (FILE *)&r_stderr_sentinel;

/**
 * @brief Interceptor for fprintf.
 *
 * Replaces fprintf. Checks if the stream is one of our
 * sentinel values. If so, redirects to R's safe printing.
 * Otherwise, passes through to standard vfprintf.
 */
int Rfprintf(FILE *stream, const char *format, ...) {
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
 */
int Rfputs(const char *s, FILE *stream) {
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
#ifdef  __cplusplus
[[noreturn]] void Rabort(void) {
    Rf_error("Filter library called abort()");
}
#else
NORET void Rabort(void) {
    Rf_error("Filter library called abort()");
}
#endif



/**
 * @brief Interceptor for exit.
 *
 * Replaces exit(). Calls R's error function instead.
 * Note: 'exit' is a 'noreturn' function. Rf_error() also
 * does not return (it longjumps), so this is a safe replacement.
 */
#ifdef  __cplusplus
[[noreturn]] void Rexit(int status) {
    Rf_error("Filter library called exit() with status %d", status);
}
#else
NORET void Rexit(int status) {
    Rf_error("Filter library called exit() with status %d", status);
}
#endif


