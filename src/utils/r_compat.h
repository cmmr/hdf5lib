/* Output and Error Interceptors */

#define R_NO_REMAP 1
#include <stdio.h>
#include <R_ext/Print.h>
#include <R_ext/Error.h>

extern FILE *Rstdout;
extern FILE *Rstderr;

#ifdef __cplusplus
extern "C" {
int Rfprintf(FILE *stream, const char *format, ...);
int Rfputs(const char *s, FILE *stream);
[[noreturn]] void Rabort(void);
[[noreturn]] void Rexit(int status);
}
#else
int Rfprintf(FILE *stream, const char *format, ...);
int Rfputs(const char *s, FILE *stream);
NORET void Rabort(void);
NORET void Rexit(int status);
#endif

