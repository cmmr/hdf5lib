/**
 * @file zstd_plugin.c
 * @brief Standalone HDF5 Filter Plugin for Zstandard (Zstd)
 */

#include <hdf5.h>
#include <zstd.h>

#define H5Z_FILTER_ZSTD 32015

#define PUSH_ERR(...) do { \
    H5Epush(H5E_DEFAULT, __FILE__, __func__, __LINE__, H5E_ERR_CLS, H5E_PLINE, H5E_CANTFILTER, __VA_ARGS__); \
    return 0; \
} while(0)

static size_t zstd_filter(
    unsigned int flags, size_t cd_nelmts, const unsigned int cd_values[], 
    size_t nbytes, size_t *buf_size, void **buf ) {
  
  /* Decompression Path */
  if (flags & H5Z_FLAG_REVERSE) {
    size_t decompSize = ZSTD_getFrameContentSize(*buf, nbytes);
    if (decompSize == ZSTD_CONTENTSIZE_ERROR || decompSize == ZSTD_CONTENTSIZE_UNKNOWN) {
      PUSH_ERR("zstd_filter: Invalid Zstd frame content size");
    }
    
    void *outbuf = H5allocate_memory(decompSize, 0);
    if (!outbuf) PUSH_ERR("zstd_filter: Memory allocation failed");
    
    size_t actual = ZSTD_decompress(outbuf, decompSize, *buf, nbytes);
    if (ZSTD_isError(actual)) {
      H5free_memory(outbuf);
      PUSH_ERR("zstd_filter: %s", ZSTD_getErrorName(actual));
    }
    H5free_memory(*buf); *buf = outbuf;
    return actual;
  }
  
  /* Compression Path */
  else {
    int aggression = (cd_nelmts > 0) ? (int)cd_values[0] : 3;
    if (aggression < ZSTD_minCLevel()) aggression = ZSTD_minCLevel();
    if (aggression > ZSTD_maxCLevel()) aggression = ZSTD_maxCLevel();
    
    size_t compLimit = ZSTD_compressBound(nbytes);
    void *outbuf = H5allocate_memory(compLimit, 0);
    if (!outbuf) PUSH_ERR("zstd_filter: Memory allocation failed");
    
    size_t compSize = ZSTD_compress(outbuf, compLimit, *buf, nbytes, aggression);
    if (ZSTD_isError(compSize)) {
      H5free_memory(outbuf);
      PUSH_ERR("zstd_filter: %s", ZSTD_getErrorName(compSize));
    }
    H5free_memory(*buf); *buf = outbuf; *buf_size = compLimit;
    return compSize;
  }
}

const H5Z_class2_t zstd_class = { H5Z_CLASS_T_VERS, H5Z_FILTER_ZSTD, 1, 1, "zstd", NULL, NULL, zstd_filter };
