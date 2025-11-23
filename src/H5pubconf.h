#ifndef H5PUBCONF_H
#define H5PUBCONF_H

/* -------------------------------------------------------------------------- */
/* 1. Runtime / Generator Definitions                                         */
/* -------------------------------------------------------------------------- */
/* Expects H5_SIZEOF_* macros and Endianness definitions from gen_config.c    */
#include "H5_sizeof.h"


/* -------------------------------------------------------------------------- */
/* 2. Project Metadata                                                        */
/* -------------------------------------------------------------------------- */
#define H5_PACKAGE_NAME "HDF5"
#define H5_PACKAGE_VERSION "2.0.0"
#define H5_PACKAGE_STRING "HDF5 2.0.0"
#define H5_PACKAGE_BUGREPORT "help@hdfgroup.org"


/* -------------------------------------------------------------------------- */
/* 3. Compiler Feature Detection                                              */
/* -------------------------------------------------------------------------- */
#if !defined(__has_include)
    #error "Your compiler is too old to build this package. GCC >= 5.0 or Clang >= 3.0 is required."
#endif

/* High Precision Float Support (HDF5 2.0+) */
/* R 4.0+ compilers (GCC/Clang) usually support __float128. */
#ifdef __SIZEOF_FLOAT128__
    #define H5_HAVE_FLOAT128 1
#endif

/* Data Representation */
/* Modern R targets (x64/ARM64) use IEEE 754 floating point. */
#define H5_HAVE_IEEE_754 1


/* -------------------------------------------------------------------------- */
/* 4. Threading Support                                                       */
/* -------------------------------------------------------------------------- */
/* Pthreads is the gold standard for R packages (even on Windows via Rtools) */
#if __has_include(<pthread.h>)
    #define H5_HAVE_PTHREAD_H 1
    #define H5_HAVE_THREADSAFE 1
    #define H5_HAVE_THREADS 1
#endif


/* -------------------------------------------------------------------------- */
/* 5. Header Availability                                                     */
/* -------------------------------------------------------------------------- */

/* A. Guaranteed Headers (R >= 4.0 / C99 / HDF5 2.0 Mandates) */
#define H5_STDC_HEADERS 1
#define H5_HAVE_STDBOOL_H 1
#define H5_HAVE_STDINT_H 1
#define H5_HAVE_STDLIB_H 1
#define H5_HAVE_STRING_H 1
#define H5_HAVE_FCNTL_H 1 
#define H5_HAVE_C99_COMPLEX_NUMBERS 1
#define H5_HAVE_COMPLEX_NUMBERS 1

/* B. Conditional Headers (POSIX & Windows) */
/* These use __has_include to be platform-agnostic */

#if __has_include(<unistd.h>)
    #define H5_HAVE_UNISTD_H 1
#endif
#if __has_include(<sys/types.h>)
    #define H5_HAVE_SYS_TYPES_H 1
#endif
#if __has_include(<sys/stat.h>)
    #define H5_HAVE_SYS_STAT_H 1
#endif
#if __has_include(<io.h>)
    #define H5_HAVE_IO_H 1
#endif
#if __has_include(<sys/time.h>)
    #define H5_HAVE_SYS_TIME_H 1
#endif
#if __has_include(<sys/resource.h>)
    #define H5_HAVE_SYS_RESOURCE_H 1
#endif
#if __has_include(<sys/file.h>)
    #define H5_HAVE_SYS_FILE_H 1
#endif
#if __has_include(<dirent.h>)
    #define H5_HAVE_DIRENT_H 1
#endif
#if __has_include(<dlfcn.h>)
    #define H5_HAVE_DLFCN_H 1
    #define H5_HAVE_LIBDL 1
#endif


/* -------------------------------------------------------------------------- */
/* 6. User Features (Hardcoded)                                               */
/* -------------------------------------------------------------------------- */
/* Warning: HL is NOT Threadsafe. Downstream packages should use locks. */
#define H5_INCLUDE_HL 1
#define H5_HAVE_FILTER_DEFLATE 1
#define H5_HAVE_ZLIB_H 1
#define H5_IGNORE_DISABLED_FILE_LOCKS 1


/* -------------------------------------------------------------------------- */
/* 7. Platform Specifics (Windows vs POSIX)                                   */
/* -------------------------------------------------------------------------- */
#if defined(_WIN32)
    /* --- Windows (Rtools / MinGW) --- */
    #define H5_HAVE_WINDOWS 1
    #define H5_HAVE_WIN32_API 1
    #define H5_HAVE_MINGW 1
    #define H5_HAVE_LIBWS2_32 1  /* Winsock needed for some internal HDF5 calls */
    #define H5_HAVE_WINDOW_PATH 1
    #define H5_HAVE_STRDUP 1
    #define H5_DEFAULT_PLUGINDIR "%ALLUSERSPROFILE%\\hdf5\\lib\\plugin"
    
    /* Force HDF5 to NOT use Win32 threads, favoring the Pthreads defined above */
    #undef H5_HAVE_WIN_THREADS 

#else
    /* --- Linux / macOS --- */
    #define H5_HAVE_ASPRINTF 1
    #define H5_HAVE_VASPRINTF 1
    #define H5_HAVE_STRCASESTR 1
    #define H5_HAVE_IOCTL 1
    #define H5_HAVE_SYMLINK 1
    #define H5_DEFAULT_PLUGINDIR "/usr/local/hdf5/lib/plugin"
    
    #ifdef __APPLE__
        #define H5_HAVE_DARWIN 1
    #endif
#endif

#endif /* H5PUBCONF_H */