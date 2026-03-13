/**
 * @file lzf_plugin.c
 * @brief Standalone HDF5 Filter Plugin for LZF
 * 
 * Fully compatible with the h5py and PyTables LZF implementations (Filter ID 32000).
 * Features 64-bit uncompressed size support and dynamic buffer expansion.
 */

/* Define BEFORE includes so lzfP.h knows to expect the state argument */
#define LZF_STATE_ARG 1

/* 
 * HACK: lzf.h hardcodes the 4-argument prototype. To avoid a compiler 
 * type-conflict when LZF_STATE_ARG is enabled, we temporarily rename 
 * the prototype using the preprocessor, include the header to get 
 * lzf_decompress, and then undefine it so we can declare the correct 
 * 5-argument prototype.
 */
#define lzf_compress lzf_compress_ignored_prototype
#include <lzf.h>
#undef lzf_compress

#include <lzfP.h>
#include <hdf5.h>
#include <stdint.h>
#include <stdlib.h>
#include <errno.h>

#ifndef LZF_VERSION
  #define LZF_VERSION 0x010500
#endif

#define H5Z_FILTER_LZF 32000
#define FILTER_LZF_VERSION 4

/* Forward-declare the correct 5-argument signature */
extern unsigned int lzf_compress(const void *const in_data, unsigned int in_len,
                                 void *out_data, unsigned int out_len, 
                                 LZF_STATE htab);

#define PUSH_ERR(...) do {                                                                               \
H5Epush(H5E_DEFAULT, __FILE__, __func__, __LINE__, H5E_ERR_CLS, H5E_PLINE, H5E_CANTFILTER, __VA_ARGS__); \
return 0;                                                                                                \
} while(0)

/* 
 * LZF requires the exact uncompressed size for decompression. 
 * We store the chunk size across cd_values[2] and [3] to support >4GB chunks.
 */
static herr_t lzf_set_local(hid_t dcpl, hid_t type, hid_t space) {
  int ndims;
  hsize_t chunkdims[32];
  unsigned int flags;
  size_t nelements = 8;
  unsigned int values[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  
  if (H5Pget_filter_by_id(dcpl, H5Z_FILTER_LZF, &flags, &nelements, values, 0, NULL, NULL) < 0) {
    return -1;
  }
  
  if (nelements < 4) nelements = 4;
  
  if (values[0] == 0) values[0] = FILTER_LZF_VERSION;
  if (values[1] == 0) values[1] = LZF_VERSION;
  
  ndims = H5Pget_chunk(dcpl, 32, chunkdims);
  if (ndims < 0) return -1;
  if (ndims > 32) {
    H5Epush(H5E_DEFAULT, __FILE__, __func__, __LINE__, H5E_ERR_CLS, H5E_PLINE, H5E_CALLBACK, "Chunk rank exceeds limit");
    return -1;
  }
  
  size_t bufsize = H5Tget_size(type);
  if (bufsize == 0) return -1;
  
  for (int i = 0; i < ndims; i++) {
    bufsize *= chunkdims[i];
  }
  
  values[2] = (unsigned int)(bufsize & 0xFFFFFFFF);
  values[3] = (unsigned int)((uint64_t)bufsize >> 32);
  
  if (H5Pmodify_filter(dcpl, H5Z_FILTER_LZF, flags, nelements, values) < 0) {
    return -1;
  }
  
  return 1;
}

static size_t lzf_filter(
    unsigned int flags, size_t cd_nelmts, const unsigned int cd_values[], 
    size_t nbytes, size_t *buf_size, void **buf) {
  
  size_t outbuf_size = 0;
  void *outbuf = NULL;
  unsigned int status = 0;

  if (nbytes == 0) {
    return 0; 
  }
  
  /* ----- Decompression Path ----- */
  if (flags & H5Z_FLAG_REVERSE) {
    
    if (cd_nelmts >= 3 && cd_values[2] != 0) {
      outbuf_size = cd_values[2];
      if (cd_nelmts >= 4) {
        outbuf_size += ((uint64_t)cd_values[3]) << 32;
      }
    } else {
      outbuf_size = (*buf_size) * 2; 
    }
    
    while (!status) {
      if (outbuf) H5free_memory(outbuf);
      
      outbuf = H5allocate_memory(outbuf_size, 0);
      if (!outbuf) PUSH_ERR("lzf_filter: Memory allocation failed");
      
      status = lzf_decompress(*buf, (unsigned int)nbytes, outbuf, (unsigned int)outbuf_size);
      
      if (!status) {
        if (errno == E2BIG) {
          outbuf_size += (*buf_size);
        } else if (errno == EINVAL) {
          H5free_memory(outbuf);
          PUSH_ERR("lzf_filter: Invalid data for LZF decompression");
        } else {
          H5free_memory(outbuf);
          PUSH_ERR("lzf_filter: Unknown LZF decompression error");
        }
      }
    }
  }
  
  /* ----- Compression Path ----- */
  else {
    outbuf_size = nbytes + (nbytes / 20) + 100;
    
    outbuf = H5allocate_memory(outbuf_size, 0);
    if (!outbuf) PUSH_ERR("lzf_filter: Memory allocation failed");
    
    /* Allocate the 512KB LZF hash table on the heap */
    void *htab = H5allocate_memory(sizeof(LZF_STATE), 1);
    if (!htab) {
      H5free_memory(outbuf);
      PUSH_ERR("lzf_filter: Hash table allocation failed");
    }
    
    status = lzf_compress(*buf, (unsigned int)nbytes, outbuf, (unsigned int)outbuf_size, (LZF_HSLOT*)htab);
    
    H5free_memory(htab);
    
    if (status == 0) {
      H5free_memory(outbuf);
      PUSH_ERR("lzf_filter: Compression failed");
    }
  }
    
  H5free_memory(*buf); 
  *buf = outbuf;
  *buf_size = outbuf_size; 
  return (size_t)status;   
}

const H5Z_class2_t lzf_class = { 
  H5Z_CLASS_T_VERS, 
  H5Z_FILTER_LZF, 
  1, 
  1, 
  "lzf", 
  NULL,          
  lzf_set_local, 
  lzf_filter 
};
