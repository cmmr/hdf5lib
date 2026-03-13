/**
 * @file blosc_plugin.c
 * @brief Standalone HDF5 Filter Plugin for Blosc
 * * Provides backwards-compatible Blosc1 compression.
 */

#include <hdf5.h>
#include <blosc2.h>
#include <stdlib.h>
#include <string.h>

#define H5Z_FILTER_BLOSC 32001
#define FILTER_BLOSC_VERSION 2
#define BLOSC_VERSION_FORMAT 2
#define BLOSC_MAX_TYPESIZE 255

/* Use the global HDF5 error class (H5E_ERR_CLS_g) so custom messages show in the trace */
#define PUSH_ERR(...) do { \
    H5Epush(H5E_DEFAULT, __FILE__, __func__, __LINE__, H5E_ERR_CLS_g, H5E_PLINE, H5E_CANTFILTER, __VA_ARGS__); \
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
    size_t nchunks, screensize, typesize;
    
    blosc_cbuffer_sizes(*buf, &nchunks, &screensize, &typesize);
    
    void *outbuf = H5allocate_memory(nchunks, 0);
    if (!outbuf) PUSH_ERR("blosc_filter: Memory allocation failed");
    
    if (blosc_decompress(*buf, outbuf, nchunks) <= 0) {
      H5free_memory(outbuf);
      PUSH_ERR("blosc_filter: Blosc decompression failed");
    }
    
    H5free_memory(*buf); 
    *buf = outbuf;
    *buf_size = nchunks;
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
    
    int compcode = 0; // Default to BLOSC_BLOSCLZ
    if (cd_nelmts >= 7) {
      compcode = cd_values[6];
    }
    
    /* Manually map the compcode to avoid string validation failures 
       for dynamically registered codecs like snappy */
    const char *compname = "blosclz";
    switch (compcode) {
        case 0: compname = "blosclz"; break;
        case 1: compname = "lz4"; break;
        case 2: compname = "lz4hc"; break;
        case 3: compname = "snappy"; break;
        case 4: compname = "zlib"; break;
        case 5: compname = "zstd"; break;
        case 6: compname = "zfp_prec"; break;
        case 11: compname = "ndlz"; break;
        default: compname = "blosclz"; break;
    }
    
    /* Pad heavily. If Blosc fails to compress, it falls back to raw memcpy, 
       requiring exactly nbytes + 16 (or 32 in Blosc2) overhead for the header. */
    size_t out_alloc = nbytes + 64; 
    void *outbuf = H5allocate_memory(out_alloc, 0);
    if (!outbuf) PUSH_ERR("blosc_filter: Memory allocation failed");
    
    if (blosc_set_compressor(compname) < 0) {
        H5free_memory(outbuf);
        PUSH_ERR("blosc_filter: Failed to set Blosc compressor: %s", compname);
    }

    /* Use the highly stable legacy macro to handle the pipeline seamlessly */
    int comp_size = blosc_compress(clevel, doshuffle, typesize, nbytes, *buf, outbuf, out_alloc);
    
    if (comp_size <= 0) {
      H5free_memory(outbuf); 
      PUSH_ERR("blosc_filter: Blosc compression failed (returned %d)", comp_size);
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
