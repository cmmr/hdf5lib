/**
 * @file blosc_plugin.c
 * @brief Standalone HDF5 Filter Plugin for Blosc
 * * Provides backwards-compatible Blosc1 compression.
 */

#include <hdf5.h>
#include <blosc.h>
#include <stdlib.h>
#include <string.h>
#include "hdf5lib.h"

#define H5Z_FILTER_BLOSC 32001
#define FILTER_BLOSC_VERSION 2
#define BLOSC_VERSION_FORMAT 2
#define BLOSC_MAX_TYPESIZE 255

/* Report through HDF5's error stack, which the caller prints (or not) through
   R's console; writing to the C library's stderr would bypass R. */
#define PUSH_ERR(...) do { \
    H5Epush(H5E_DEFAULT, __FILE__, __func__, __LINE__, H5E_ERR_CLS, H5E_PLINE, H5E_CANTFILTER, __VA_ARGS__); \
    return 0; \
} while(0)

/* c-blosc's *_ctx() functions start and join their worker threads on every
   call, which costs more than the threads save on chunks of a megabyte or so.
   Only its global context keeps a pool alive between calls, so use that when
   more than one thread is wanted.  blosc_compress() lets the variables below
   override the clevel, shuffle, codec, etc. it is given, so compress through
   the global context only while none of them is set; blosc_decompress() takes
   nothing from the environment but BLOSC_NTHREADS.  (HDF5 makes every filter
   call under its global lock, so the global compressor setting cannot change
   under a call in progress.) */
static int use_global_pool(int nthreads, int compressing) {
  static const char *overrides[] = {
    "BLOSC_CLEVEL", "BLOSC_SHUFFLE", "BLOSC_TYPESIZE",
    "BLOSC_COMPRESSOR", "BLOSC_BLOCKSIZE", "BLOSC_SPLITMODE" };

  if (nthreads <= 1) return 0;
  if (compressing) {
    for (size_t i = 0; i < sizeof(overrides) / sizeof(overrides[0]); i++)
      if (getenv(overrides[i]) != NULL) return 0;
  }
  if (blosc_get_nthreads() != nthreads) blosc_set_nthreads(nthreads);
  return 1;
}

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
    size_t uncomp_size;

    /* blosc_decompress() trusts the sizes in the chunk's header, so make sure
       the header describes exactly the nbytes HDF5 read */
    if (blosc_cbuffer_validate(*buf, nbytes, &uncomp_size) < 0)
      PUSH_ERR("Blosc chunk of %zu bytes is truncated or corrupt", nbytes);
    
    void *outbuf = H5allocate_memory(uncomp_size, 0);
    if (!outbuf) PUSH_ERR("Memory allocation failed for %zu bytes", uncomp_size);
    
    int nthreads = hdf5lib_get_blosc_nthreads();
    int status = use_global_pool(nthreads, 0)
      ? blosc_decompress(*buf, outbuf, uncomp_size)
      : blosc_decompress_ctx(*buf, outbuf, uncomp_size, nthreads);
    
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
    if (cd_nelmts < 4) PUSH_ERR("Blosc requires at least 4 cd_values");
    
    size_t typesize = cd_values[2];
    int clevel = (cd_nelmts >= 5) ? (int)cd_values[4] : 5;
    int doshuffle = (cd_nelmts >= 6) ? (int)cd_values[5] : 1;
    int compcode = (cd_nelmts >= 7) ? cd_values[6] : 0;
    
    const char *compname = "unknown";
    if (blosc_compcode_to_compname(compcode, &compname) == -1) {
        PUSH_ERR("This Blosc library build does not support compressor code: %d", compcode);
    }
    
    /* 1. Allocate a massive shock-absorber.
       Snappy's C API lacks bounds checking and its worst-case expansion is 
       roughly nbytes + (nbytes / 6) + 32. 
       We allocate 1.5x + 1KB to guarantee it can never overflow the heap. */
    size_t out_alloc = nbytes + (nbytes / 2) + 1024; 
    void *outbuf = H5allocate_memory(out_alloc, 0);
    if (!outbuf) PUSH_ERR("Memory allocation failed for compression");
    
    int comp_size;
    int nthreads = hdf5lib_get_blosc_nthreads();
    if (use_global_pool(nthreads, 1)) {
        blosc_set_compressor(compname);
        blosc_set_blocksize(0);
        comp_size = blosc_compress(clevel, doshuffle, typesize, nbytes, *buf, outbuf, out_alloc);
    } else {
        comp_size = blosc_compress_ctx(clevel, doshuffle, typesize, nbytes, *buf, outbuf, out_alloc, compname, 0, nthreads);
    }
    
    if (comp_size <= 0) {
        H5free_memory(outbuf);
        return 0; 
    }
    
    /* Since we massively overallocated, we must ensure HDF5 knows the *actual* size to write to disk. Returning comp_size does exactly this. */
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
