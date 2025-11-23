#ifndef H5PUBCONF_H
#define H5PUBCONF_H

/* ========================================================================== */
/* 1. Runtime / Generator Definitions                                       */
/* ========================================================================== */
#include "H5_sizeof.h"


/* ========================================================================== */
/* 2. Project Metadata & Policy                                             */
/* ========================================================================== */
#define H5_PACKAGE_NAME "HDF5"
#define H5_PACKAGE_VERSION "2.0.0"
#define H5_PACKAGE_STRING "HDF5 2.0.0"
#define H5_PACKAGE_BUGREPORT "help@hdfgroup.org"
#define H5_PACKAGE "hdf5"
#define H5_PACKAGE_TARNAME "hdf5"
#define H5_PACKAGE_URL "https://www.hdfgroup.org"
#define H5_VERSION "2.0.0"

#define H5_USE_FILE_LOCKING 1
#define H5_IGNORE_DISABLED_FILE_LOCKS 1
#define H5_HAVE_EMBEDDED_LIBINFO 1
#define H5_WANT_DATA_ACCURACY 1
#define H5_WANT_DCONV_EXCEPTION 1
#define H5_INCLUDE_HL 1


/* ========================================================================== */
/* 3. Compiler & Feature Detection                                          */
/* ========================================================================== */
#if !defined(__has_include)
    #error "Your compiler is too old to build this package. GCC >= 5.0 or Clang >= 3.0 is required."
#endif

#define H5_HAVE_ATTRIBUTE 1
#define H5_HAVE_C99_COMPLEX_NUMBERS 1
#define H5_HAVE_COMPLEX_NUMBERS 1
#define H5_STDC_HEADERS 1
#define H5_HAVE_IEEE_754 1

#ifdef __SIZEOF_FLOAT128__
    #define H5_HAVE_FLOAT128 1
#endif

/* Endianness Fallback */
#ifndef WORDS_BIGENDIAN
    #if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && \
        __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        #define WORDS_BIGENDIAN 1
    #endif
#endif


/* ========================================================================== */
/* 4. Threading Support                                                     */
/* ========================================================================== */
#if __has_include(<pthread.h>)
    #define H5_HAVE_PTHREAD_H 1
    #define H5_HAVE_THREADSAFE 1
    #define H5_HAVE_THREADS 1
#endif


/* ========================================================================== */
/* 5. Header Availability                                                   */
/* ========================================================================== */
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
#if __has_include(<sys/socket.h>)
    #define H5_HAVE_SYS_SOCKET_H 1
#endif
#if __has_include(<sys/ioctl.h>)
    #define H5_HAVE_SYS_IOCTL_H 1
#endif
#if __has_include(<dirent.h>)
    #define H5_HAVE_DIRENT_H 1
#endif
#if __has_include(<dlfcn.h>)
    #define H5_HAVE_DLFCN_H 1
    #define H5_HAVE_LIBDL 1
#endif
#if __has_include(<stdatomic.h>)
    #define H5_HAVE_STDATOMIC_H 1
#endif
#if __has_include(<netdb.h>)
    #define H5_HAVE_NETDB_H 1
#endif
#if __has_include(<arpa/inet.h>)
    #define H5_HAVE_ARPA_INET_H 1
#endif
#if __has_include(<netinet/in.h>)
    #define H5_HAVE_NETINET_IN_H 1
#endif
#if __has_include(<pwd.h>)
    #define H5_HAVE_PWD_H 1
#endif


/* ========================================================================== */
/* 6. Filter & Compression Support                                          */
/* ========================================================================== */
#define H5_HAVE_FILTER_DEFLATE 1
#define H5_HAVE_ZLIB_H 1
#define H5_HAVE_LIBZ 1
#define H5_HAVE_LIBM 1


/* ========================================================================== */
/* 7. Platform Specifics                                                    */
/* ========================================================================== */

/* Truly Universal (MinGW + POSIX) */
#define H5_HAVE_STRDUP 1
#define H5_HAVE_GETTIMEOFDAY 1
#define H5_HAVE_TIMEZONE 1
#define H5_HAVE_TMPFILE 1
#define H5_HAVE_CLOCK_GETTIME 1
#define H5_HAVE_FCNTL 1

/* --- Windows Specifics (Rtools / MinGW) --- */
#if defined(_WIN32)
    #define H5_HAVE_WINDOWS 1
    #define H5_HAVE_WIN32_API 1
    #define H5_HAVE_MINGW 1
    #define H5_HAVE_LIBWS2_32 1
    #define H5_HAVE_WINDOW_PATH 1
    #define H5_HAVE_GETCONSOLESCREENBUFFERINFO 1
    
    #define H5_DEFAULT_PLUGINDIR "%ALLUSERSPROFILE%\\hdf5\\lib\\plugin"
    
    /* Use Pthreads, not Win32 threads */
    #undef H5_HAVE_WIN_THREADS 
    
    /* MinGW lacks these, so HDF5 must provide replacements */
    #undef H5_HAVE_ASPRINTF
    #undef H5_HAVE_VASPRINTF

/* --- POSIX Specifics (Linux / macOS) --- */
#else
    #define H5_HAVE_ALARM 1
    #define H5_HAVE_FORK 1
    #define H5_HAVE_WAITPID 1
    #define H5_HAVE_GETHOSTNAME 1
    #define H5_HAVE_GETRUSAGE 1
    #define H5_HAVE_FLOCK 1
    #define H5_HAVE_SYMLINK 1
    #define H5_HAVE_PREADWRITE 1
    #define H5_HAVE_STRCASESTR 1
    #define H5_HAVE_TM_GMTOFF 1
    #define H5_HAVE_STAT_ST_BLOCKS 1
    #define H5_HAVE_QSORT_REENTRANT 1
    
    /* These exist on POSIX */
    #define H5_HAVE_ASPRINTF 1
    #define H5_HAVE_VASPRINTF 1

    #define H5_HAVE_IOCTL 1
    #define H5_HAVE_TIOCGETD 1
    #define H5_HAVE_TIOCGWINSZ 1

    #define H5_DEFAULT_PLUGINDIR "/usr/local/hdf5/lib/plugin"
    
    #if defined(__APPLE__)
        #define H5_HAVE_DARWIN 1
    #endif
#endif

#endif /* H5PUBCONF_H */