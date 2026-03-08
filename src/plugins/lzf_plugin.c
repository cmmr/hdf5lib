/**
 * @file lzf_plugin.c
 * @brief Standalone HDF5 Filter Plugin for LZF
 * 
 * Fully compatible with the h5py LZF implementation (Filter ID 32000).
 */

#include <hdf5.h>
#include <lzf.h>
#include <stdint.h>
#include <stdlib.h>

#define H5Z_FILTER_LZF 32000
#define H5PY_FILTER_LZF_VERSION 4
#define H5PY_LZF_MAGIC_VERSION 0x010500

#define PUSH_ERR(...) do {                                                                               \
H5Epush(H5E_DEFAULT, __FILE__, __func__, __LINE__, H5E_ERR_CLS, H5E_PLINE, H5E_CANTFILTER, __VA_ARGS__); \
return 0;                                                                                                \
} while(0)

/* 
 * LZF requires the exact uncompressed size for decompression. 
 * h5py handles this by storing the chunk size in the filter's cd_values.
 */
static herr_t lzf_set_local(hid_t dcpl, hid_t type, hid_t space) {
  int ndims;
  hsize_t chunkdims[32];
  unsigned int flags;
  size_t nelements = 8;
  unsigned int values[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  
  /* Retrieve existing filter settings */
  if (H5Pget_filter_by_id(dcpl, H5Z_FILTER_LZF, &flags, &nelements, values, 0, NULL, NULL) < 0) {
    return -1;
  }
  
  /* Set h5py compatibility version info */
  values[0] = H5PY_FILTER_LZF_VERSION;
  values[1] = H5PY_LZF_MAGIC_VERSION;
  
  /* Calculate the uncompressed chunk size in bytes */
  ndims = H5Pget_chunk(dcpl, 32, chunkdims);
  if (ndims < 0) return -1;
  
  size_t typesize = H5Tget_size(type);
  if (typesize == 0) return -1;
  
  size_t bufsize = typesize;
  for (int i = 0; i < ndims; i++) {
    bufsize *= chunkdims[i];
  }
  
  /* Store the chunk size in slots 2 and 3 (for 64-bit sizes) */
  values[2] = (unsigned int)(bufsize & 0xFFFFFFFF);
  values[3] = (unsigned int)((uint64_t)bufsize >> 32);
  
  /* Update the Property List with the new values */
  if (H5Pmodify_filter(dcpl, H5Z_FILTER_LZF, flags, 4, values) < 0) {
    return -1;
  }
  
  return 1;
}

static size_t lzf_filter(
    unsigned int flags, size_t cd_nelmts, const unsigned int cd_values[], 
                                                                      size_t nbytes, size_t *buf_size, void **buf) {
  
  /* ----- Decompression Path ----- */
  if (flags & H5Z_FLAG_REVERSE) {
    size_t outbuf_size = 0;
    
    /* Extract the uncompressed size from the metadata injected by set_local */
    if (cd_nelmts >= 3 && cd_values[2] != 0) {
      outbuf_size = cd_values[2];
      if (cd_nelmts >= 4) {
        outbuf_size += ((uint64_t)cd_values[3]) << 32;
      }
    } else {
      PUSH_ERR("lzf_filter: Missing uncompressed size in cd_values. Corrupt or incompatible LZF data.");
    }
    
    /* Delegate allocation to HDF5 as seen in Blosc, LZ4, and Zstd */
    void *outbuf = H5allocate_memory(outbuf_size, 0);
    if (!outbuf) PUSH_ERR("lzf_filter: Memory allocation failed");
    
    unsigned int status = lzf_decompress(*buf, (unsigned int)nbytes, outbuf, (unsigned int)outbuf_size);
    if (status == 0) {
      H5free_memory(outbuf);
      PUSH_ERR("lzf_filter: LZF decompression failed");
    }
    
    H5free_memory(*buf); 
    *buf = outbuf;
    return (size_t)status; /* Return the uncompressed size */
  }
  
  /* ----- Compression Path ----- */
  else {
    /* Provide a slightly padded buffer for the compressor (+5% overhead safety margin) */
    size_t outbuf_size = nbytes + (nbytes / 20) + 100;
    
    void *outbuf = H5allocate_memory(outbuf_size, 0);
    if (!outbuf) PUSH_ERR("lzf_filter: Memory allocation failed");
    
    unsigned int comp_size = lzf_compress(*buf, (unsigned int)nbytes, outbuf, (unsigned int)outbuf_size);
    
    /* If data is incompressible, lzf_compress returns 0 */
    if (comp_size == 0 || comp_size >= nbytes) {
      H5free_memory(outbuf);
      return 0; /* HDF5 will natively store the chunk uncompressed */
    }
    
    H5free_memory(*buf); 
    *buf = outbuf;
    *buf_size = outbuf_size; /* HDF5 tracks the allocated buffer size */
    return (size_t)comp_size;
  }
}

/* Register the filter class with the set_local callback explicitly enabled */
const H5Z_class2_t lzf_class = { 
  H5Z_CLASS_T_VERS, 
  H5Z_FILTER_LZF, 
  1, 
  1, 
  "lzf", 
  NULL,          /* can_apply */
lzf_set_local, /* set_local */
lzf_filter 
};
