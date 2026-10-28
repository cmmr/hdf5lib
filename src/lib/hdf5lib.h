#ifndef HDF5LIB_FILTERS_H
#define HDF5LIB_FILTERS_H
#include <hdf5.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Expose the functions to downstream packages */
herr_t hdf5lib_register_all_filters(void);
herr_t hdf5lib_destroy_all_filters(void);

/* Threads the Blosc and Blosc2 filters use to (de)compress the blocks of a
   chunk.  The default is one.  A BLOSC_NTHREADS environment variable takes
   precedence over hdf5lib_set_blosc_nthreads(), as it does throughout c-blosc,
   but the setter is the better choice: c-blosc2 also applies the variable to
   bookkeeping contexts it makes for every chunk, which on Windows start and
   stop threads of their own.  The setter takes values from 1 to 256, or 0 to
   drop back to the default, and returns the previous setting (0 if none was
   made).  The getter returns the number of threads the filters will use. */
int hdf5lib_set_blosc_nthreads(int nthreads);
int hdf5lib_get_blosc_nthreads(void);

#ifdef __cplusplus
}
#endif

#endif