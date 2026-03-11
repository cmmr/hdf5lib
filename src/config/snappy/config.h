#ifndef THIRD_PARTY_SNAPPY_OPENSOURCE_CMAKE_CONFIG_H_
#define THIRD_PARTY_SNAPPY_OPENSOURCE_CMAKE_CONFIG_H_

/* Bring in the R build environment definitions, including WORDS_BIGENDIAN */
#include "H5pubconf.h"

/* --- Endianness Mapping --- */
#ifdef WORDS_BIGENDIAN
#define SNAPPY_IS_BIG_ENDIAN 1
#else
#define SNAPPY_IS_BIG_ENDIAN 0
#endif

/* --- Compiler Features --- */
/* Modern GCC/Clang (R >= 4.0) supports all of these built-ins */
#define HAVE_ATTRIBUTE_ALWAYS_INLINE 1
#define HAVE_BUILTIN_CTZ 1
#define HAVE_BUILTIN_EXPECT 1
#define HAVE_BUILTIN_PREFETCH 1

/* --- OS/Platform Specifics (Mapped from H5pubconf.h) --- */
#if defined(H5_HAVE_WINDOWS)
    #define HAVE_WINDOWS_H 1
    #define HAVE_FUNC_MMAP 0
    #define HAVE_FUNC_SYSCONF 0
    #define HAVE_SYS_MMAN_H 0
    #define HAVE_SYS_RESOURCE_H 0
    #define HAVE_SYS_UIO_H 0
#else
    #define HAVE_WINDOWS_H 0
    #define HAVE_FUNC_MMAP 1
    #define HAVE_FUNC_SYSCONF 1
    #define HAVE_SYS_MMAN_H 1
    #define HAVE_SYS_RESOURCE_H 1
    #define HAVE_SYS_UIO_H 1
#endif

/* Universally available in R build environments per H5pubconf.h */
#define HAVE_SYS_TIME_H 1
#define HAVE_UNISTD_H 1

/* --- Libraries --- */
#define HAVE_LIBLZO2 0
#define HAVE_LIBZ 1     /* Bundled/Linked in H5pubconf.h */
#define HAVE_LIBLZ4 1   /* You are bundling this */

/* --- Hardware Acceleration --- */
/* 
 * Set these to 0 for CRAN safety. Snappy has standard C++ fallbacks 
 * for everything. Hardcoding 1 here could break builds on ARM/M1 Macs 
 * or older x86 machines without specific vector instructions. 
 */
#define SNAPPY_HAVE_SSSE3 0
#define SNAPPY_HAVE_X86_CRC32 0
#define SNAPPY_HAVE_BMI2 0
#define SNAPPY_HAVE_NEON 0
#define SNAPPY_HAVE_NEON_CRC32 0

#endif  // THIRD_PARTY_SNAPPY_OPENSOURCE_CMAKE_CONFIG_H_
