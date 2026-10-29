/**
 * @file bshuf_plugin.c
 * @brief Standalone HDF5 Filter Plugin for Bitshuffle (ID 32008)
 * * Includes support for pure bitshuffling, internal LZ4, and Zstd compression.
 */

#include <hdf5.h>
#include <bitshuffle.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>


int64_t bshuf_compress_zstd(const void* in, void* out, const size_t size, 
                            const size_t elem_size, const size_t block_size, const int comp_lvl);
int64_t bshuf_decompress_zstd(const void* in, void* out, const size_t size, 
                              const size_t elem_size, const size_t block_size);
size_t bshuf_compress_zstd_bound(const size_t size, const size_t elem_size, const size_t block_size);


#ifndef BSHUF_VERSION_MAJOR
  #define BSHUF_VERSION_MAJOR 0
  #define BSHUF_VERSION_MINOR 5
  #define BSHUF_VERSION_POINT 2
#endif

#define BSHUF_H5FILTER 32008

/* Bitshuffle internal compression algorithm IDs */
#define BSHUF_H5_COMPRESS_NONE 0
#define BSHUF_H5_COMPRESS_LZ4  2
#define BSHUF_H5_COMPRESS_ZSTD 3

#define PUSH_ERR(...) do { \
    H5Epush(H5E_DEFAULT, __FILE__, __func__, __LINE__, H5E_ERR_CLS, H5E_PLINE, H5E_CANTFILTER, __VA_ARGS__); \
    return 0; \
} while(0)

/* --- CRAN-safe Big-Endian Encoders (Avoids OS-specific headers) --- */
static void write_be64(uint8_t *p, uint64_t val) {
  p[0] = (uint8_t)(val >> 56); p[1] = (uint8_t)(val >> 48);
  p[2] = (uint8_t)(val >> 40); p[3] = (uint8_t)(val >> 32);
  p[4] = (uint8_t)(val >> 24); p[5] = (uint8_t)(val >> 16);
  p[6] = (uint8_t)(val >> 8);  p[7] = (uint8_t)(val & 0xFF);
}

static uint64_t read_be64(const uint8_t *p) {
  return ((uint64_t)p[0] << 56) | ((uint64_t)p[1] << 48) | ((uint64_t)p[2] << 40) | ((uint64_t)p[3] << 32) |
         ((uint64_t)p[4] << 24) | ((uint64_t)p[5] << 16) | ((uint64_t)p[6] << 8)  | ((uint64_t)p[7]);
}

static void write_be32(uint8_t *p, uint32_t val) {
  p[0] = (uint8_t)(val >> 24); p[1] = (uint8_t)(val >> 16);
  p[2] = (uint8_t)(val >> 8);  p[3] = (uint8_t)(val & 0xFF);
}

static uint32_t read_be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | ((uint32_t)p[3]);
}

/* Block sizes are a multiple of this (BSHUF_BLOCKED_MULT in bitshuffle's
   internal headers) */
#define BSHUF_BLOCK_MULT 8

/* bshuf_decompress_lz4() and _zstd() take no input length: each block's
   compressed size comes from the 4-byte big-endian header in front of it.
   Walk those headers the way bshuf_blocked_wrap_fun() does -- full blocks,
   then one partial block of a multiple of 8 elements, then the last
   (num_elems % 8) elements copied as they are -- so that a truncated or
   corrupt chunk is refused instead of read past its end. */
static int bshuf_blocks_fit(const uint8_t *in, size_t avail, size_t num_elems,
                            size_t elem_size, size_t block_size) {
  size_t nblocks = num_elems / block_size;
  size_t last = num_elems % block_size;
  size_t pos = 0;
  if (last - last % BSHUF_BLOCK_MULT) nblocks++;
  for (size_t b = 0; b < nblocks; b++) {
    if (avail - pos < 4) return 0;
    uint32_t len = read_be32(in + pos);
    if (len > INT32_MAX) return 0;  /* bitshuffle reads it as an int32_t */
    pos += 4;
    if (avail - pos < len) return 0;
    pos += len;
  }
  return avail - pos >= (num_elems % BSHUF_BLOCK_MULT) * elem_size;
}

/* * Callback to intercept dataset types and populate cd_values.
 */
static herr_t bshuf_set_local(hid_t dcpl, hid_t type, hid_t space) {
  unsigned int flags;
  size_t nelements = 8;
  unsigned int tmp_values[8] = {0};
  unsigned int values[11] = {0};

  if (H5Pget_filter_by_id(dcpl, BSHUF_H5FILTER, &flags, &nelements, tmp_values, 0, NULL, NULL) < 0) {
    return -1;
  }

  /* Shift user-provided options (block_size, compress_algo, comp_lvl) right by
     3 -- unless this function already filled the list in (a copied DCPL, as
     h5repack makes), which then starts with bitshuffle's version: major 0 and
     a minor of 4 or more, where a user's second value is a codec (0, 2 or 3).
     Shifting that again would make the minor version the codec. */
  size_t shift = (nelements >= 3 && tmp_values[0] == 0 && tmp_values[1] >= 4) ? 0 : 3;
  for (size_t i = 0; i < nelements && i + shift < 11; i++) {
    values[i + shift] = tmp_values[i];
  }
  nelements += shift;

  /* Refuse what every chunk would fail on (bitshuffle error -81), while
     H5Dcreate2() can still report it */
  if (nelements > 3 && values[3] % BSHUF_BLOCK_MULT) {
    H5Epush(H5E_DEFAULT, __FILE__, __func__, __LINE__, H5E_ERR_CLS, H5E_PLINE, H5E_BADVALUE,
            "bitshuffle: block size must be a multiple of %d, not %u", BSHUF_BLOCK_MULT, values[3]);
    return -1;
  }
  if (nelements > 4 && values[4] != BSHUF_H5_COMPRESS_NONE &&
      values[4] != BSHUF_H5_COMPRESS_LZ4 && values[4] != BSHUF_H5_COMPRESS_ZSTD) {
    H5Epush(H5E_DEFAULT, __FILE__, __func__, __LINE__, H5E_ERR_CLS, H5E_PLINE, H5E_BADVALUE,
            "bitshuffle: compression must be 0 (none), 2 (lz4) or 3 (zstd), not %u", values[4]);
    return -1;
  }

  /* Slot 0 & 1: Versioning */
  values[0] = BSHUF_VERSION_MAJOR;
  values[1] = BSHUF_VERSION_MINOR;

  /* Slot 2: Element Size */
  size_t elem_size = H5Tget_size(type);
  if (elem_size == 0) return -1;

  /* Unwrap arrays to get base element size */
  H5T_class_t classt = H5Tget_class(type);
  if (classt == H5T_ARRAY) {
    hid_t super_type = H5Tget_super(type);
    elem_size = H5Tget_size(super_type);
    H5Tclose(super_type);
  }
  values[2] = (unsigned int)elem_size;

  if (H5Pmodify_filter(dcpl, BSHUF_H5FILTER, flags, nelements, values) < 0) {
    return -1;
  }

  return 1;
}

static size_t bshuf_filter(
    unsigned int flags, size_t cd_nelmts, const unsigned int cd_values[], 
    size_t nbytes, size_t *buf_size, void **buf) {
  
  if (nbytes == 0) return 0;
  
  if (cd_nelmts < 3) {
    PUSH_ERR("bshuf_filter: Not enough parameters (missing element size)");
  }

  size_t elem_size = cd_values[2];
  size_t block_size = (cd_nelmts > 3) ? cd_values[3] : 0;
  int comp_algo = (cd_nelmts > 4) ? cd_values[4] : BSHUF_H5_COMPRESS_NONE;

  if (elem_size == 0) PUSH_ERR("bshuf_filter: Element size of 0 in filter parameters");

  if (block_size == 0) {
    block_size = bshuf_default_block_size(elem_size);
  }

  /* Like the reference filter, treat any other codec ID as plain bitshuffle:
     datasets it wrote that way exist, and it reads them so */
  if (comp_algo != BSHUF_H5_COMPRESS_LZ4 && comp_algo != BSHUF_H5_COMPRESS_ZSTD) {
    comp_algo = BSHUF_H5_COMPRESS_NONE;
  }

  /* ----- Decompression Path ----- */
  if (flags & H5Z_FLAG_REVERSE) {
    size_t nbytes_uncomp;
    const uint8_t *in_buf = (const uint8_t *)*buf;
    
    if (comp_algo == BSHUF_H5_COMPRESS_LZ4 || comp_algo == BSHUF_H5_COMPRESS_ZSTD) {
      if (nbytes < 12) PUSH_ERR("bshuf_filter: Corrupt data, missing header");
      
      nbytes_uncomp = read_be64(in_buf);
      size_t header_block_bytes = read_be32(in_buf + 8);
      block_size = header_block_bytes / elem_size;
      in_buf += 12; /* Skip header */
      if (block_size == 0) block_size = bshuf_default_block_size(elem_size);
    } else {
      nbytes_uncomp = nbytes; /* Pure bitshuffle, sizes are identical */
    }

    if (nbytes_uncomp % elem_size != 0) {
      PUSH_ERR("bshuf_filter: Uncompressed size is not a multiple of element size");
    }

    size_t num_elems = nbytes_uncomp / elem_size;

    if ((comp_algo == BSHUF_H5_COMPRESS_LZ4 || comp_algo == BSHUF_H5_COMPRESS_ZSTD) &&
        (block_size % BSHUF_BLOCK_MULT != 0 ||
         !bshuf_blocks_fit(in_buf, nbytes - 12, num_elems, elem_size, block_size))) {
      PUSH_ERR("bshuf_filter: Chunk is truncated or corrupt");
    }

    void *out_buf = H5allocate_memory(nbytes_uncomp, 0);
    if (!out_buf) PUSH_ERR("bshuf_filter: Memory allocation failed");

    int64_t err = -1;
    if (comp_algo == BSHUF_H5_COMPRESS_LZ4) {
      err = bshuf_decompress_lz4(in_buf, out_buf, num_elems, elem_size, block_size);
    } else if (comp_algo == BSHUF_H5_COMPRESS_ZSTD) {
      err = bshuf_decompress_zstd(in_buf, out_buf, num_elems, elem_size, block_size);
    } else {
      err = bshuf_bitunshuffle(in_buf, out_buf, num_elems, elem_size, block_size);
    }

    if (err < 0) {
      H5free_memory(out_buf);
      PUSH_ERR("bshuf_filter: Bitshuffle decompression failed");
    }

    H5free_memory(*buf);
    *buf = out_buf;
    *buf_size = nbytes_uncomp;
    
    return nbytes_uncomp;
  }
  
  /* ----- Compression Path ----- */
  else {
    if (nbytes % elem_size != 0) {
      PUSH_ERR("bshuf_filter: Input size is not a multiple of element size");
    }
    
    size_t num_elems = nbytes / elem_size;
    size_t out_alloc;
    
    if (comp_algo == BSHUF_H5_COMPRESS_LZ4) {
      out_alloc = bshuf_compress_lz4_bound(num_elems, elem_size, block_size) + 12;
    } else if (comp_algo == BSHUF_H5_COMPRESS_ZSTD) {
      out_alloc = bshuf_compress_zstd_bound(num_elems, elem_size, block_size) + 12;
    } else {
      out_alloc = nbytes; /* Pure bitshuffle bound */
    }

    void *out_buf = H5allocate_memory(out_alloc, 0);
    if (!out_buf) PUSH_ERR("bshuf_filter: Memory allocation failed");

    int64_t err = -1;
    size_t final_out_size = 0;

    if (comp_algo == BSHUF_H5_COMPRESS_LZ4 || comp_algo == BSHUF_H5_COMPRESS_ZSTD) {
      write_be64((uint8_t *)out_buf, (uint64_t)nbytes);
      write_be32((uint8_t *)out_buf + 8, (uint32_t)(block_size * elem_size));
      
      void *comp_dest = (uint8_t *)out_buf + 12;
      
      if (comp_algo == BSHUF_H5_COMPRESS_LZ4) {
        err = bshuf_compress_lz4(*buf, comp_dest, num_elems, elem_size, block_size);
      } else {
        int comp_lvl = (cd_nelmts > 5) ? (int)cd_values[5] : 0;
        err = bshuf_compress_zstd(*buf, comp_dest, num_elems, elem_size, block_size, comp_lvl);
      }
      final_out_size = (size_t)err + 12;
    } else {
      err = bshuf_bitshuffle(*buf, out_buf, num_elems, elem_size, block_size);
      final_out_size = nbytes;
    }

    if (err < 0) {
      H5free_memory(out_buf);
      PUSH_ERR("bshuf_filter: Bitshuffle compression failed");
    }

    H5free_memory(*buf);
    *buf = out_buf;
    *buf_size = out_alloc;
    return final_out_size;
  }
}

const H5Z_class2_t bshuf_class = { 
  H5Z_CLASS_T_VERS, 
  BSHUF_H5FILTER, 
  1, 
  1, 
  "bitshuffle", 
  NULL, 
  bshuf_set_local, 
  bshuf_filter 
};
