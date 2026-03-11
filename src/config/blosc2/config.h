#ifndef _CONFIGURATION_HEADER_GUARD_H_
#define _CONFIGURATION_HEADER_GUARD_H_

/* Standard compressors we're bundling natively */
#define HAVE_LZ4 1
#define HAVE_ZSTD 1

/* Support original blosc API */
#define BLOSC1_COMPAT 1

/* Safely map ZLIB from HDF5's configuration */
#include "H5pubconf.h"
#if defined(H5_HAVE_LIBZ) || defined(H5_HAVE_ZLIB_H)
#define HAVE_ZLIB 1
#else
#define HAVE_ZLIB 0
#endif

/* Disabled or unsafe features */
#define HAVE_ZLIB_NG 0
#define HAVE_IPP 0
#define HAVE_PLUGINS 0
#define BLOSC_DLL_EXPORT

#endif /* _CONFIGURATION_HEADER_GUARD_H_ */