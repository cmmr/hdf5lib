/**
 * @file bzip2_plugin.c
 * @brief Standalone HDF5 Filter Plugin for BZIP2
 * 
 * Compatible with standard PyTables/HDF5 bzip2 filter implementations (Filter ID 307).
 */

#include <hdf5.h>
#include <bzlib.h>
#include <stdlib.h>

#define H5Z_FILTER_BZIP2 307

#define PUSH_ERR(...) do {                                                                               \
H5Epush(H5E_DEFAULT, __FILE__, __func__, __LINE__, H5E_ERR_CLS, H5E_PLINE, H5E_CANTFILTER, __VA_ARGS__); \
return 0;                                                                                                \
} while(0)

static size_t bzip2_filter(
    unsigned int flags, size_t cd_nelmts, const unsigned int cd_values[], 
    size_t nbytes, size_t *buf_size, void **buf) {
  
  /* Handle completely empty dataset/chunk edge case */
  if (nbytes == 0) {
    return 0;
  }

  /* ----- Decompression Path ----- */
  if (flags & H5Z_FLAG_REVERSE) {
    bz_stream strm;
    strm.bzalloc = NULL;
    strm.bzfree = NULL;
    strm.opaque = NULL;
    strm.next_in = (char *)*buf;
    
    /* bzlib's avail_in is an unsigned int. Limit inputs to 4GB. */
    if (nbytes > 0xFFFFFFFF) {
      PUSH_ERR("bzip2_filter: Compressed chunk exceeds 4GB bzlib limit");
    }
    strm.avail_in = (unsigned int)nbytes;
    
    if (BZ2_bzDecompressInit(&strm, 0, 0) != BZ_OK) {
      PUSH_ERR("bzip2_filter: BZ2_bzDecompressInit failed");
    }
    
    /* Guess a 3x compression ratio to start, minimum 64KB */
    size_t out_alloc = nbytes * 3;
    if (out_alloc < 65536) out_alloc = 65536;
    
    void *outbuf = H5allocate_memory(out_alloc, 0);
    if (!outbuf) {
      BZ2_bzDecompressEnd(&strm);
      PUSH_ERR("bzip2_filter: Memory allocation failed");
    }
    
    strm.next_out = (char *)outbuf;
    /* Cap avail_out at ~1GB to avoid unsigned int overflow in bzlib */
    strm.avail_out = (out_alloc > 0x3FFFFFFF) ? 0x3FFFFFFF : (unsigned int)out_alloc;
    
    int ret;
    while ((ret = BZ2_bzDecompress(&strm)) == BZ_OK) {
      /* If the output buffer is full, double it and continue */
      if (strm.avail_out == 0) {
        size_t current_out = (size_t)(strm.next_out - (char *)outbuf);
        size_t new_alloc = out_alloc * 2;
        
        void *newbuf = H5resize_memory(outbuf, new_alloc);
        if (!newbuf) {
          BZ2_bzDecompressEnd(&strm);
          H5free_memory(outbuf);
          PUSH_ERR("bzip2_filter: Memory reallocation failed");
        }
        outbuf = newbuf;
        out_alloc = new_alloc;
        
        strm.next_out = (char *)outbuf + current_out;
        size_t left = out_alloc - current_out;
        strm.avail_out = (left > 0x3FFFFFFF) ? 0x3FFFFFFF : (unsigned int)left;
      }
    }
    
    if (ret != BZ_STREAM_END) {
      BZ2_bzDecompressEnd(&strm);
      H5free_memory(outbuf);
      PUSH_ERR("bzip2_filter: Decompression failed with error code %d", ret);
    }
    
    /* Calculate final uncompressed size using pointer arithmetic rather than 
       stream.total_out_lo32 to safely support chunks > 4GB */
    size_t final_size = (size_t)(strm.next_out - (char *)outbuf);
    BZ2_bzDecompressEnd(&strm);
    
    H5free_memory(*buf);
    *buf = outbuf;
    *buf_size = out_alloc; /* HDF5 tracks the total allocated buffer size */
    return final_size;
  }
  
  /* ----- Compression Path ----- */
  else {
    /* bzlib's buffer-to-buffer compression uses unsigned int lengths. Limit to 4GB. */
    if (nbytes > 0xFFFFFFFF) {
      PUSH_ERR("bzip2_filter: Uncompressed chunk exceeds 4GB bzlib limit");
    }
    
    /* BZIP2 accepts an aggressive level from 1 (fastest) to 9 (best). Default to 9. */
    int block_size_100k = 9;
    if (cd_nelmts > 0 && cd_values[0] >= 1 && cd_values[0] <= 9) {
      block_size_100k = (int)cd_values[0];
    }
    
    /* BZIP2 guarantees output will never exceed 1% + 600 bytes larger than input */
    size_t comp_limit = nbytes + (nbytes / 100) + 600;
    
    void *outbuf = H5allocate_memory(comp_limit, 0);
    if (!outbuf) {
      PUSH_ERR("bzip2_filter: Memory allocation failed");
    }
    
    unsigned int dest_len = (unsigned int)comp_limit;
    int ret = BZ2_bzBuffToBuffCompress((char *)outbuf, &dest_len, 
                                       (char *)*buf, (unsigned int)nbytes, 
                                       block_size_100k, 0, 0);
    
    if (ret != BZ_OK) {
      H5free_memory(outbuf);
      PUSH_ERR("bzip2_filter: Compression failed with error code %d", ret);
    }
    
    H5free_memory(*buf);
    *buf = outbuf;
    *buf_size = comp_limit;
    return (size_t)dest_len;
  }
}

const H5Z_class2_t bzip2_class = { 
  H5Z_CLASS_T_VERS, 
  H5Z_FILTER_BZIP2, 
  1, 
  1, 
  "bzip2", 
  NULL, 
  NULL, 
  bzip2_filter 
};
