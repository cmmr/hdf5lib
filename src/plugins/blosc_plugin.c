/**
 * @file blosc_plugin.c
 * @brief Standalone HDF5 Filter Plugin for Blosc
 */

#include <hdf5.h>
#include <blosc.h>
#include <string.h>

#define H5Z_FILTER_BLOSC 32001

#define PUSH_ERR(...) do { \
    H5Epush(H5E_DEFAULT, __FILE__, __func__, __LINE__, H5E_ERR_CLS, H5E_PLINE, H5E_CANTFILTER, __VA_ARGS__); \
    return 0; \
} while(0)

static size_t blosc_filter(
    unsigned int flags, size_t cd_nelmts, const unsigned int cd_values[], 
    size_t nbytes, size_t *buf_size, void **buf) {
  
  /* ----- Decompression Path ----- */
  if (flags & H5Z_FLAG_REVERSE) {
    size_t nchunks, screensize, typesize;
    blosc_cbuffer_sizes(*buf, &nchunks, &screensize, &typesize);
    
    void *outbuf = H5allocate_memory(nchunks, 0);
    if (!outbuf) PUSH_ERR("blosc_filter: Memory allocation failed");
    
    if (blosc_decompress(*buf, outbuf, nchunks) <= 0) {
      H5free_memory(outbuf);
      PUSH_ERR("blosc_filter: Blosc decompression failed");
    }
    H5free_memory(*buf); *buf = outbuf;
    return nchunks;
  }
  
  /* ----- Compression Path ----- */
  else {
    if (cd_nelmts < 4) {
      PUSH_ERR("blosc_filter: Blosc requires at least 4 cd_values");
    }
    
    size_t typesize = cd_values[2];
    int clevel = (cd_nelmts >= 5) ? (int)cd_values[4] : 5;
    int doshuffle = (cd_nelmts >= 6) ? (int)cd_values[5] : 1;
    const char* compname = "blosclz";
    
    /* Optional: Select internal Blosc compressor (Zstd, LZ4, etc.) */
    if (cd_nelmts >= 7) {
      int compcode = cd_values[6];
      if (blosc_compcode_to_compname(compcode, &compname) == -1) {
        PUSH_ERR("blosc_filter: Requested Blosc compressor not available");
      }
    }
    
    /* Allocate strictly nbytes. If compression inflates data, we fallback to uncompressed. */
    void *outbuf = H5allocate_memory(nbytes, 0);
    if (!outbuf) PUSH_ERR("blosc_filter: Memory allocation failed");
    
    blosc_set_compressor(compname);
    int comp_size = blosc_compress(clevel, doshuffle, typesize, nbytes, *buf, outbuf, nbytes);
    
    if (comp_size <= 0) {
      /* Data was incompressible. Free buffer and return 0. HDF5 will store it uncompressed. */
      H5free_memory(outbuf); 
      return 0;
    }
    
    H5free_memory(*buf); *buf = outbuf;
    return (size_t)comp_size;
  }
}

/* Note: blosc_set_local was removed. Set the 7th property to NULL */
const H5Z_class2_t blosc_class = { 
  H5Z_CLASS_T_VERS, H5Z_FILTER_BLOSC, 1, 1, "blosc", NULL, NULL, blosc_filter 
};
