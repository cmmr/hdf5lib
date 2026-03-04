/* lz4_config.h. Generated manually for CRAN R >= 4.2 compatibility. */

#ifndef LZ4_CONFIG_H
#define LZ4_CONFIG_H

/* Network byte order headers (for htonl / ntohl) */
#if defined(_WIN32) || defined(__MINGW32__)
  /* Windows/Rtools uses winsock2 for network byte operations */
  #define HAVE_WINSOCK2_H 1
  #undef HAVE_ARPA_INET_H
  #undef HAVE_NETINET_IN_H
#else
  /* macOS, Linux, and Solaris use standard POSIX network headers */
  #undef HAVE_WINSOCK2_H
  #define HAVE_ARPA_INET_H 1
  #define HAVE_NETINET_IN_H 1
#endif

/* Define to 1 if you have the <dlfcn.h> header file. */
/* POSIX only; absent on Windows. */
#undef HAVE_DLFCN_H

/* Define to 1 if you don't have `vprintf' but do have `_doprnt.' */
#undef HAVE_DOPRNT

/* Define to 1 if you have the <fcntl.h> header file. */
/* Excluded as H5pubconf.h limits this to POSIX environments. */
#undef HAVE_FCNTL_H

/* Define to 1 if you have the <hdf5.h> header file. */
#define HAVE_HDF5_H 1

/* Define to 1 if you have the <inttypes.h> header file. */
/* Guaranteed by C99 standard in R 4.2+ */
#define HAVE_INTTYPES_H 1

/* Define to 1 if you have the `lz4' library (-llz4). */
#define HAVE_LIBLZ4 1

/* Define to 1 if you have the `m' library (-lm). */
#define HAVE_LIBM 1

/* Define to 1 if you have the <lz4.h> header file. */
#define HAVE_LZ4_H 1

/* Define to 1 if you have the <memory.h> header file. */
/* Deprecated/legacy; use <string.h> instead. */
#undef HAVE_MEMORY_H

/* Define to 1 if you have the `memset' function. */
#define HAVE_MEMSET 1

/* Define to 1 if you have the <stdint.h> header file. */
/* Guaranteed by C99 standard in R 4.2+ */
#define HAVE_STDINT_H 1

/* Define to 1 if you have the <stdlib.h> header file. */
#define HAVE_STDLIB_H 1

/* Define to 1 if you have the <strings.h> header file. */
/* POSIX specific; stick to standard <string.h> for universal support. */
#undef HAVE_STRINGS_H

/* Define to 1 if you have the <string.h> header file. */
#define HAVE_STRING_H 1

/* Define to 1 if you have the <sys/stat.h> header file. */
/* Universally available across CRAN environments per H5pubconf.h. */
#define HAVE_SYS_STAT_H 1

/* Define to 1 if you have the <sys/types.h> header file. */
/* Universally available across CRAN environments per H5pubconf.h. */
#define HAVE_SYS_TYPES_H 1

/* Define to 1 if you have the <unistd.h> header file. */
/* Universally available across CRAN environments per H5pubconf.h. */
#define HAVE_UNISTD_H 1

/* Define to 1 if you have the `vprintf' function. */
#define HAVE_VPRINTF 1

/* Define to the sub-directory where libtool stores uninstalled libraries. */
/* Unnecessary for R's build system (Makevars / R CMD SHLIB). */
#undef LT_OBJDIR

/* Name of package */
#define PACKAGE "hdf5-lz4-plugin"

/* Define to the address where bug reports for this package should be sent. */
#define PACKAGE_BUGREPORT "dansmith01@gmail.com"

/* Define to the full name of this package. */
#define PACKAGE_NAME "HDF5 LZ4 Plugin"

/* Define to the full name and version of this package. */
#define PACKAGE_STRING "HDF5 LZ4 Plugin 2.1.0"

/* Define to the one symbol short name of this package. */
#define PACKAGE_TARNAME "hdf5-lz4-plugin"

/* Define to the home page for this package. */
#define PACKAGE_URL "https://github.com/HDFGroup/hdf5_plugins"

/* Define to the version of this package. */
#define PACKAGE_VERSION "2.1.0"

/* Define to 1 if you have the ANSI C header files. */
#define STDC_HEADERS 1

/* Version number of package */
#define VERSION "2.1.0"

/* Define to empty if `const' does not conform to ANSI C. */
/* C99 supports const natively. */
/* #undef const */

/* Define to `unsigned int' if <sys/types.h> does not define. */
/* size_t is universally defined in modern std headers. */
/* #undef size_t */

#endif /* LZ4_CONFIG_H */
