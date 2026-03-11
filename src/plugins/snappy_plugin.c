/**
 * @file snappy_plugin.c
 * @brief Standalone HDF5 Filter Plugin for Google Snappy
 * 
 * Uses the snappy-c.h wrapper over the C++ Snappy library.
 * Registered HDF5 Filter ID: 32003
 */

#include <hdf5.h>
#include <snappy-c.h>
#include <stdlib.h>

#define H5Z_FILTER_SNAPPY 32003

#define PUSH_ERR(...) do { \
    H5Epush(H5E_DEFAULT, __FILE__, __func__, __LINE__, H5E_ERR_CLS, H5E_PLINE, H5E_CANTFILTER, __VA_ARGS__); \
    return 0; \
} while(0)

static size_t snappy_filter(
    unsigned int flags, size_t cd_nelmts, const unsigned int cd_values[], 
    size_t nbytes, size_t *buf_size, void **buf) {
  
  /* Handle completely empty dataset/chunk edge case */
  if (nbytes == 0) {
    return 0;
  }

  /* Snappy's internal block format relies on 32-bit integers for length. 
     We must prevent chunks larger than ~4GB from crashing the compressor. */
  if (nbytes > 0xFFFFFFFF) {
    PUSH_ERR("snappy_filter: Chunk exceeds 4GB Snappy limit");
  }

  /* ----- Decompression Path ----- */
  if (flags & H5Z_FLAG_REVERSE) {
    size_t uncomp_len = 0;
    snappy_status status;
    
    /* Snappy embeds the uncompressed length in the compressed stream header */
    status = snappy_uncompressed_length((const char *)*buf, nbytes, &uncomp_len);
    if (status != SNAPPY_OK) {
      PUSH_ERR("snappy_filter: Failed to parse uncompressed length from Snappy header");
    }
    
    void *outbuf = H5allocate_memory(uncomp_len, 0);
    if (!outbuf) PUSH_ERR("snappy_filter: Memory allocation failed");
    
    status = snappy_uncompress((const char *)*buf, nbytes, (char *)outbuf, &uncomp_len);
    if (status != SNAPPY_OK) {
      H5free_memory(outbuf);
      PUSH_ERR("snappy_filter: Snappy decompression failed");
    }
    
    H5free_memory(*buf);
    *buf = outbuf;
    *buf_size = uncomp_len;
    return uncomp_len;
  }
  
  /* ----- Compression Path ----- */
  else {
    /* Get the maximum possible size the compressed data could take */
    size_t comp_limit = snappy_max_compressed_length(nbytes);
    
    void *outbuf = H5allocate_memory(comp_limit, 0);
    if (!outbuf) PUSH_ERR("snappy_filter: Memory allocation failed");
    
    size_t comp_len = comp_limit;
    snappy_status status = snappy_compress((const char *)*buf, nbytes, (char *)outbuf, &comp_len);
    
    if (status != SNAPPY_OK) {
      H5free_memory(outbuf);
      PUSH_ERR("snappy_filter: Snappy compression failed");
    }
    
    /* If data is incompressible, tell HDF5 to store it natively */
    if (comp_len == 0 || comp_len >= nbytes) {
      H5free_memory(outbuf);
      return 0;
    }
    
    H5free_memory(*buf);
    *buf = outbuf;
    *buf_size = comp_limit; /* HDF5 tracks the allocated buffer size */
    return comp_len;
  }
}

/* Register the filter class */
const H5Z_class2_t snappy_class = { 
  H5Z_CLASS_T_VERS, 
  H5Z_FILTER_SNAPPY, 
  1, 
  1, 
  "snappy", 
  NULL,  /* can_apply */
  NULL,  /* set_local (Not needed for Snappy) */
  snappy_filter 
};
