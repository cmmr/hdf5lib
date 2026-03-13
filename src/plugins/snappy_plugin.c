/**
 * @file snappy_plugin.c
 * @brief Standalone HDF5 Filter Plugin for Google Snappy
 * 
 * Uses the pure C csnappy port.
 * Registered HDF5 Filter ID: 32003
 */

#include <hdf5.h>
#include "csnappy.h"
#include <stdlib.h>
#include <stdint.h>

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
    uint32_t uncomp_len = 0;
    int status;
    
    /* Snappy embeds the uncompressed length in the compressed stream header */
    status = csnappy_get_uncompressed_length((const char *)*buf, (uint32_t)nbytes, &uncomp_len);
    if (status == CSNAPPY_E_HEADER_BAD) {
      PUSH_ERR("snappy_filter: Failed to parse uncompressed length from Snappy header");
    }
    
    void *outbuf = H5allocate_memory(uncomp_len, 0);
    if (!outbuf) PUSH_ERR("snappy_filter: Memory allocation failed");
    
    status = csnappy_decompress((const char *)*buf, (uint32_t)nbytes, (char *)outbuf, uncomp_len);
    if (status != CSNAPPY_E_OK) {
      H5free_memory(outbuf);
      PUSH_ERR("snappy_filter: Snappy decompression failed");
    }
    
    H5free_memory(*buf);
    *buf = outbuf;
    *buf_size = (size_t)uncomp_len;
    return (size_t)uncomp_len;
  }
  
  /* ----- Compression Path ----- */
  else {
    /* Get the maximum possible size the compressed data could take */
    uint32_t comp_limit = csnappy_max_compressed_length((uint32_t)nbytes);
    
    void *outbuf = H5allocate_memory(comp_limit, 0);
    if (!outbuf) PUSH_ERR("snappy_filter: Memory allocation failed");

    /* csnappy requires a dedicated working memory buffer for compression */
    void *workmem = H5allocate_memory(CSNAPPY_WORKMEM_BYTES, 0);
    if (!workmem) {
        H5free_memory(outbuf);
        PUSH_ERR("snappy_filter: Working memory allocation failed");
    }
    
    uint32_t comp_len = 0;
    csnappy_compress((const char *)*buf, (uint32_t)nbytes, (char *)outbuf, &comp_len, workmem, CSNAPPY_WORKMEM_BYTES_POWER_OF_TWO);
    
    H5free_memory(workmem);
    
    /* Strictly check for 0 (error) */
    if (comp_len == 0) {
      H5free_memory(outbuf);
      PUSH_ERR("snappy_filter: Snappy compression failed");
    }
    
    H5free_memory(*buf);
    *buf = outbuf;
    *buf_size = (size_t)comp_limit; /* HDF5 tracks the allocated buffer size */
    return (size_t)comp_len;
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
