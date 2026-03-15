/**
 * @file blosc_plugin.c
 * @brief Standalone HDF5 Filter Plugin for Blosc
 * * Provides backwards-compatible Blosc1 compression.
 */

#include <hdf5.h>
#include <blosc2.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h> /* Required for stderr/fprintf */

#define H5Z_FILTER_BLOSC 32001
#define FILTER_BLOSC_VERSION 2
#define BLOSC_VERSION_FORMAT 2
#define BLOSC_MAX_TYPESIZE 255

/* Force the error message directly to the console before HDF5 has a chance to swallow it */
#define PUSH_ERR(...) do { \
    fprintf(stderr, "\n>>> [BLOSC_PLUGIN ERROR at line %d]: ", __LINE__); \
    fprintf(stderr, __VA_ARGS__); \
    fprintf(stderr, " <<<\n"); \
    fflush(stderr); \
    H5Epush(H5E_DEFAULT, __FILE__, __func__, __LINE__, H5E_ERR_CLS_g, H5E_PLINE, H5E_CANTFILTER, "Filter failed"); \
    return 0; \
} while(0)

static herr_t blosc_set_local(hid_t dcpl, hid_t type, hid_t space) {
  unsigned int flags;
  size_t cd_nelmts = 8;
  unsigned int cd_values[8] = {0, 0, 0, 0, 0, 0, 0, 0};

  if (H5Pget_filter_by_id(dcpl, H5Z_FILTER_BLOSC, &flags, &cd_nelmts, cd_values, 0, NULL, NULL) < 0) {
    return -1;
  }

  if (cd_nelmts < 4) cd_nelmts = 4;

  cd_values[0] = FILTER_BLOSC_VERSION;
  cd_values[1] = BLOSC_VERSION_FORMAT;

  size_t typesize = H5Tget_size(type);
  if (typesize == 0) return -1;

  H5T_class_t classt = H5Tget_class(type);
  if (classt == H5T_ARRAY) {
    hid_t super_type = H5Tget_super(type);
    typesize = H5Tget_size(super_type);
    H5Tclose(super_type);
  }

  if (typesize > BLOSC_MAX_TYPESIZE) {
    typesize = 1;
  }
  cd_values[2] = (unsigned int)typesize;

  int ndims;
  hsize_t chunkdims[32];
  ndims = H5Pget_chunk(dcpl, 32, chunkdims);
  if (ndims < 0) return -1;

  size_t chunksize = H5Tget_size(type);
  for (int i = 0; i < ndims; i++) {
    chunksize *= chunkdims[i];
  }
  cd_values[3] = (unsigned int)(chunksize & 0xFFFFFFFF);

  if (H5Pmodify_filter(dcpl, H5Z_FILTER_BLOSC, flags, cd_nelmts, cd_values) < 0) {
    return -1;
  }

  return 1;
}

static size_t blosc_filter(
    unsigned int flags, size_t cd_nelmts, const unsigned int cd_values[], 
    size_t nbytes, size_t *buf_size, void **buf) {
  
  if (nbytes == 0) {
    return 0; 
  }

  /* ----- Decompression Path ----- */
  if (flags & H5Z_FLAG_REVERSE) {
    size_t uncomp_size, screensize, typesize;
    
    blosc_cbuffer_sizes(*buf, &uncomp_size, &screensize, &typesize);
    
    void *outbuf = H5allocate_memory(uncomp_size, 0);
    if (!outbuf) PUSH_ERR("Memory allocation failed for %zu bytes", uncomp_size);
    
    /* Use context engine for decompression to tap into rich error states */
    blosc2_dparams dparams = BLOSC2_DPARAMS_DEFAULTS;
    blosc2_context *dctx = blosc2_create_dctx(dparams);
    if (!dctx) {
        H5free_memory(outbuf);
        PUSH_ERR("Failed to create Blosc2 decompression context");
    }

    int status = blosc2_decompress_ctx(dctx, *buf, (int32_t)nbytes, outbuf, (int32_t)uncomp_size);
    blosc2_free_ctx(dctx);
    
    if (status <= 0) {
        H5free_memory(outbuf);
        
        /* Extract Format ID (not compcode) */
        uint8_t *p = (uint8_t *)*buf;
        int compformat = (p[2] >> 5) & 7;
        const char *compname = "unknown";
        
        if (compformat == 0) compname = "blosclz";
        else if (compformat == 1) compname = "lz4 / lz4hc";
        else if (compformat == 2) compname = "snappy";
        else if (compformat == 3) compname = "zlib";
        else if (compformat == 4) compname = "zstd";
        else if (compformat == 6) compname = "zfp";

        PUSH_ERR("Decompression failed (status %d). Chunk requires codec format: %s (format id %d). Is this plugin enabled?", status, compname, compformat);
    }
    
    H5free_memory(*buf); 
    *buf = outbuf;
    *buf_size = uncomp_size;
    return uncomp_size;
  }
  
  /* ----- Compression Path ----- */
  else {
    if (cd_nelmts < 4) {
      PUSH_ERR("Blosc requires at least 4 cd_values");
    }
    
    size_t typesize = cd_values[2];
    int clevel = (cd_nelmts >= 5) ? (int)cd_values[4] : 5;
    int doshuffle = (cd_nelmts >= 6) ? (int)cd_values[5] : 1;
    int compcode = 0; // Default BLOSC_BLOSCLZ
    
    if (cd_nelmts >= 7) {
      compcode = cd_values[6];
    }
    
    size_t out_alloc = nbytes + 64; 
    void *outbuf = H5allocate_memory(out_alloc, 0);
    if (!outbuf) PUSH_ERR("Memory allocation failed for compression");
    
    int comp_size = 0;

    blosc2_cparams cparams = BLOSC2_CPARAMS_DEFAULTS;
    cparams.compcode = compcode;
    cparams.clevel = clevel;
    cparams.typesize = (int32_t)typesize;
    cparams.blocksize = 0;
    
    if (doshuffle == 1) cparams.filters[5] = BLOSC_SHUFFLE;
    else if (doshuffle == 2) cparams.filters[5] = BLOSC_BITSHUFFLE;
    else cparams.filters[5] = BLOSC_NOSHUFFLE;

    blosc2_context *cctx = blosc2_create_cctx(cparams);
    if (cctx) {
        comp_size = blosc2_compress_ctx(cctx, *buf, (int32_t)nbytes, outbuf, (int32_t)out_alloc);
        blosc2_free_ctx(cctx);
    }
    
    if (comp_size <= 0) {
        uint8_t *p = (uint8_t *)outbuf;
        
        p[0] = FILTER_BLOSC_VERSION;
        p[1] = 1;                   
        p[2] = (uint8_t)(0x10 | ((compcode & 7) << 5)); 
        p[3] = (uint8_t)typesize;   
        
        uint32_t uncomp_bytes = (uint32_t)nbytes;
        uint32_t header_len = 16;
        uint32_t final_bytes = uncomp_bytes + header_len;
        
        #define W_LE32(dest, val) do { \
            (dest)[0] = (uint8_t)((val) & 0xFF); \
            (dest)[1] = (uint8_t)(((val) >> 8) & 0xFF); \
            (dest)[2] = (uint8_t)(((val) >> 16) & 0xFF); \
            (dest)[3] = (uint8_t)(((val) >> 24) & 0xFF); \
        } while(0)
        
        W_LE32(p + 4, uncomp_bytes);  
        W_LE32(p + 8, uncomp_bytes);  
        W_LE32(p + 12, final_bytes);  
        
        memcpy(p + 16, *buf, nbytes);
        comp_size = (int)final_bytes;
    }
    
    H5free_memory(*buf); 
    *buf = outbuf;
    *buf_size = out_alloc;
    return (size_t)comp_size;
  }
}

const H5Z_class2_t blosc_class = { 
  H5Z_CLASS_T_VERS, 
  H5Z_FILTER_BLOSC, 
  1, 
  1, 
  "blosc", 
  NULL, 
  blosc_set_local, 
  blosc_filter 
};
