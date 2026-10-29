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

/* Add the Blosc2 filter (32026) to a dataset creation property list, or
   update the one already there, building its cd_values from:
     flags       H5Z_FLAG_MANDATORY or H5Z_FLAG_OPTIONAL, as for H5Pset_filter()
     codec       one of the HDF5LIB_BLOSC2_* codecs below
     level       compression level, 0 to 9
     prefilters  the HDF5LIB_BLOSC2_* pre-filters below, ORed together; they run
                 in the order trunc-prec, delta, then bitshuffle or shuffle
     meta        trunc-prec's precision (bits to keep, or a negative number of
                 bits to drop), NDLZ's cell size (4 or 8), or the ZFP codecs'
                 parameter (accuracy 10^meta, precision in bits, or rate as a
                 percentage of the input size); otherwise 0
   Returns a negative value, with the reason on HDF5's error stack, for a level,
   pre-filter or meta value out of range.  The codec, and how the settings suit
   the data, are checked when H5Dcreate2() applies the filter. */
#define HDF5LIB_BLOSC2_BLOSCLZ      0
#define HDF5LIB_BLOSC2_LZ4          1
#define HDF5LIB_BLOSC2_LZ4HC        2
#define HDF5LIB_BLOSC2_ZLIB         4
#define HDF5LIB_BLOSC2_ZSTD         5
#define HDF5LIB_BLOSC2_NDLZ        32
#define HDF5LIB_BLOSC2_ZFP_ACC     33
#define HDF5LIB_BLOSC2_ZFP_PREC    34
#define HDF5LIB_BLOSC2_ZFP_RATE    35

#define HDF5LIB_BLOSC2_SHUFFLE      1
#define HDF5LIB_BLOSC2_BITSHUFFLE   2
#define HDF5LIB_BLOSC2_DELTA        4
#define HDF5LIB_BLOSC2_TRUNC_PREC   8

herr_t hdf5lib_set_blosc2(hid_t plist, unsigned int flags, unsigned int codec,
                          unsigned int level, unsigned int prefilters, int meta);

/* Add the ZFP filter (32013) to a dataset creation property list, replacing
   any ZFP filter already there, in one of its modes: a fixed rate (bits per
   value), precision (bits kept), accuracy (largest absolute error), expert
   parameters, or reversible (lossless).  As in LLNL's H5Z-ZFP, whose names
   they keep, each adds the filter as mandatory; H5Pmodify_filter(plist, 32013,
   H5Z_FLAG_OPTIONAL, 0, NULL) makes it optional. */
herr_t H5Pset_zfp_rate(hid_t plist, double rate);
herr_t H5Pset_zfp_precision(hid_t plist, unsigned int prec);
herr_t H5Pset_zfp_accuracy(hid_t plist, double acc);
herr_t H5Pset_zfp_expert(hid_t plist, unsigned int minbits, unsigned int maxbits,
                         unsigned int maxprec, int minexp);
herr_t H5Pset_zfp_reversible(hid_t plist);

#ifdef __cplusplus
}
#endif

#endif