/**
 * @file blosc2_plugin.c
 * @brief Standalone HDF5 Filter Plugin for Blosc2 (Filter ID 32026)
 * Incorporates community best-practices for memory management while 
 * retaining custom filters_meta support for dynamic ZFP and TruncPrec codecs.
 */

#include <hdf5.h>
#include <blosc2.h>
#include <b2nd.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>

#define FILTER_BLOSC2 32026
#define FILTER_BLOSC2_VERSION 1
#define DEFAULT_CLEVEL 5
#define DEFAULT_SHUFFLE 1
#define DEFAULT_COMPCODE BLOSC_BLOSCLZ
/* Increased by 1 to hold the filters_meta argument */
#define MAX_FILTER_VALUES (9 + BLOSC2_MAX_DIM) 

#define B2ND_OPAQUE_NPDTYPE_FORMAT "|V%zd"
#define B2ND_OPAQUE_NPDTYPE_MAXLEN (2 + 20 + 1)

#define PUSH_ERR(...) do { \
    H5Epush(H5E_DEFAULT, __FILE__, __func__, __LINE__, H5E_ERR_CLS, H5E_PLINE, H5E_CANTFILTER, __VA_ARGS__); \
    return 0; \
} while(0)

/* =========================================================================
 * SET_LOCAL CALLBACK (METADATA INJECTION)
 * ========================================================================= */
static herr_t blosc2_set_local(hid_t dcpl, hid_t type, hid_t space) {
  int ndim, i;
  herr_t r;
  unsigned int typesize, basetypesize, bufsize;
  hsize_t chunkshape[H5S_MAX_RANK];
  unsigned int flags;
  size_t nelements = MAX_FILTER_VALUES;
  unsigned int values[MAX_FILTER_VALUES];

  memset(values, 0, sizeof(values));
  r = H5Pget_filter_by_id(dcpl, FILTER_BLOSC2, &flags, &nelements, values, 0, NULL, NULL);
  if (r < 0) return -1;

  /* Capture user-provided filters_meta if 8 parameters were passed */
  unsigned int user_meta = 0;
  if (nelements >= 8) {
      user_meta = values[7];
  }

  if (nelements < 4) nelements = 4;

  values[0] = FILTER_BLOSC2_VERSION;

  ndim = H5Pget_chunk(dcpl, H5S_MAX_RANK, chunkshape);
  if (ndim < 0) return -1;
  if (ndim > H5S_MAX_RANK) {
    H5Epush(H5E_DEFAULT, __FILE__, __func__, __LINE__, H5E_ERR_CLS, H5E_PLINE, H5E_CALLBACK, "Chunk rank exceeds HDF5 limit");
    return -1;
  }

  typesize = (unsigned int)H5Tget_size(type);
  if (typesize == 0) return -1;

  H5T_class_t classt = H5Tget_class(type);
  if (classt == H5T_ARRAY) {
    hid_t super_type = H5Tget_super(type);
    basetypesize = (unsigned int)H5Tget_size(super_type);
    H5Tclose(super_type);
  } else {
    basetypesize = typesize;
  }

  values[2] = basetypesize;

  bufsize = typesize;
  for (i = 0; i < ndim; i++) {
    bufsize *= (unsigned int)chunkshape[i];
  }
  values[3] = bufsize;

  if (1 < ndim && ndim <= BLOSC2_MAX_DIM) {
    if (nelements < 5) values[4] = DEFAULT_CLEVEL;
    if (nelements < 6) values[5] = DEFAULT_SHUFFLE;
    if (nelements < 7) values[6] = DEFAULT_COMPCODE;

    values[7] = ndim;
    for (int j = 0; j < ndim; j++) {
      values[8 + j] = (unsigned int)(chunkshape[j]);
    }
    
    /* Shift filters_meta to the end of the b2nd chunk block */
    values[8 + ndim] = user_meta;
    nelements = 9 + ndim;
  } else {
    if (nelements < 5) values[4] = DEFAULT_CLEVEL;
    if (nelements < 6) values[5] = DEFAULT_SHUFFLE;
    if (nelements < 7) values[6] = DEFAULT_COMPCODE;
    
    /* Safely assign filters_meta for 1D chunks */
    values[7] = user_meta;
    nelements = 8;
  }

  if (H5Pmodify_filter(dcpl, FILTER_BLOSC2, flags, nelements, values) < 0) return -1;
  return 1;
}

/* =========================================================================
 * BLOCK SIZE HEURISTICS
 * ========================================================================= */
static int32_t compute_blosc2_blocksize(int32_t chunksize, int32_t typesize, int clevel, int compcode) {
  static uint8_t data_dest[BLOSC2_MAX_OVERHEAD];
  blosc2_cparams cparams = BLOSC2_CPARAMS_DEFAULTS;
  cparams.compcode = (compcode < 0) ? DEFAULT_COMPCODE : compcode;
  cparams.clevel = clevel;
  cparams.typesize = typesize;

  if (blosc2_chunk_zeros(cparams, chunksize, data_dest, BLOSC2_MAX_OVERHEAD) < 0) return -1;

  int32_t blocksize = -1;
  if (blosc2_cbuffer_sizes(data_dest, NULL, NULL, &blocksize) < 0) return -1;
  return blocksize;
}

static int32_t compute_b2nd_block_shape(size_t block_size, size_t type_size, const int rank, const int32_t *dims_chunk, int32_t *dims_block) {
  size_t nitems = block_size / type_size;
  size_t nitems_new = 1;
  for (int i = 0; i < rank; i++) {
    dims_block[i] = dims_chunk[i] == 1 ? 1 : 2;
    nitems_new *= dims_block[i];
  }

  if (nitems_new >= nitems) return (int32_t)(nitems_new * type_size);

  while (nitems_new < nitems) {
    size_t nitems_prev = nitems_new;
    for (int i = rank - 1; i >= 0; i--) {
      if (dims_block[i] * 2 <= dims_chunk[i]) {
        if (nitems_new * 2 <= nitems) {
          nitems_new *= 2;
          dims_block[i] *= 2;
        }
      } else if (dims_block[i] < dims_chunk[i]) {
        size_t newitems_ext = (nitems_new / dims_block[i]) * dims_chunk[i];
        if (newitems_ext <= nitems) {
          nitems_new = newitems_ext;
          dims_block[i] = dims_chunk[i];
        }
      }
    }
    if (nitems_new == nitems_prev) break; 
  }
  return (int32_t)(nitems_new * type_size);
}

/* =========================================================================
 * CORE FILTER IMPLEMENTATION
 * ========================================================================= */
static size_t blosc2_filter_function(
    unsigned int flags, size_t cd_nelmts, const unsigned int cd_values[], 
    size_t nbytes, size_t *buf_size, void **buf) {

  if (nbytes == 0) return 0;

  void *outbuf = NULL;
  int64_t status = 0;
  size_t blocksize, typesize, outbuf_size;
  int clevel = DEFAULT_CLEVEL;
  int doshuffle = DEFAULT_SHUFFLE;
  int compcode = DEFAULT_COMPCODE;
  int meta = 0; 
  char errmsg[256];

  if (cd_nelmts < 4) PUSH_ERR("blosc2_filter: Too few filter parameters");

  blocksize = cd_values[1]; 
  typesize = cd_values[2]; 
  outbuf_size = cd_values[3]; 

  int ndim = -1;
  int32_t chunkshape[BLOSC2_MAX_DIM];
  
  if (cd_nelmts >= 8) {
      ndim = cd_values[7];
      if (ndim >= 1 && ndim <= BLOSC2_MAX_DIM) {
          if (cd_nelmts >= (size_t)(9 + ndim)) {
              meta = cd_values[8 + ndim];
          }
          for (int i = 0; i < ndim; i++) chunkshape[i] = (int32_t)cd_values[8 + i];
      } else {
          meta = cd_values[7];
          ndim = -1;
      }
  }

  /* ----- Compression Path ----- */
  if (!(flags & H5Z_FLAG_REVERSE)) {
    if (cd_nelmts >= 5) clevel = cd_values[4];
    if (cd_nelmts >= 6) doshuffle = cd_values[5];
    if (cd_nelmts >= 7) {
        compcode = cd_values[6];
        /* Skip static verification for Dynamic Custom Codecs (ZFP=6, NDLZ=11) */
        if (compcode < 6) {
            const char *compname;
            if (blosc2_compcode_to_compname(compcode, &compname) == -1) {
                snprintf(errmsg, sizeof(errmsg), "blosc2_filter: Compressor %d not supported.", compcode);
                PUSH_ERR(errmsg);
            }
        }
    }

    blosc2_cparams cparams = BLOSC2_CPARAMS_DEFAULTS;
    cparams.compcode = compcode;
    cparams.typesize = (int32_t)typesize;
    cparams.filters[BLOSC_LAST_FILTER] = doshuffle;
    cparams.filters_meta[BLOSC_LAST_FILTER] = meta; // Explicit Meta Injection
    cparams.clevel = clevel;

    blosc2_storage storage = {.cparams = &cparams, .contiguous = false};

    /* Multi-Dimensional (B2ND) Chunking */
    if (ndim > 1) {
      b2nd_context_t *ctx = NULL;
      b2nd_array_t *array = NULL;

      if (blocksize == 0) {
        int32_t sugg = compute_blosc2_blocksize((int32_t)outbuf_size, (int32_t)typesize, clevel, compcode);
        if (sugg < 0) PUSH_ERR("blosc2_filter: Failed to compute suggested blocksize");
        blocksize = sugg;
      }
      
      int32_t blockdims[BLOSC2_MAX_DIM];
      cparams.blocksize = compute_b2nd_block_shape(blocksize, typesize, ndim, chunkshape, blockdims);

      int64_t chunkshape_l[BLOSC2_MAX_DIM];
      for (int i = 0; i < ndim; i++) chunkshape_l[i] = chunkshape[i];

      char dtype[B2ND_OPAQUE_NPDTYPE_MAXLEN];
      snprintf(dtype, sizeof(dtype), B2ND_OPAQUE_NPDTYPE_FORMAT, typesize);
      
      if (!(ctx = b2nd_create_ctx(&storage, ndim, chunkshape_l, chunkshape, blockdims, dtype, DTYPE_NUMPY_FORMAT, NULL, 0))) {
        PUSH_ERR("blosc2_filter: Cannot create B2ND context");
      }

      if (b2nd_from_cbuffer(ctx, &array, *buf, (int32_t)nbytes) < 0) {
        b2nd_free_ctx(ctx);
        PUSH_ERR("blosc2_filter: Cannot compress buffer into B2ND array");
      }

      bool needs_free = false;
      uint8_t *tmp_out = NULL;
      if (b2nd_to_cframe(array, &tmp_out, &status, &needs_free) < 0) {
        b2nd_free(array); b2nd_free_ctx(ctx);
        PUSH_ERR("blosc2_filter: Cannot convert B2ND array to buffer");
      }

      if (status <= 0) {
          if (needs_free && tmp_out) free(tmp_out);
          b2nd_free(array); 
          b2nd_free_ctx(ctx);
          PUSH_ERR("blosc2_filter: B2ND compression failed");
      }

      /* Safely transfer to HDF5 managed memory */
      outbuf = H5allocate_memory((size_t)status, 0);
      memcpy(outbuf, tmp_out, (size_t)status);
      
      if (needs_free && tmp_out) free(tmp_out);
      b2nd_free(array);
      b2nd_free_ctx(ctx);
    } 
    /* 1D Linear Chunking */
    else {
      cparams.blocksize = (int32_t)blocksize;

      blosc2_context *cctx = blosc2_create_cctx(cparams);
      blosc2_schunk *schunk = blosc2_schunk_new(&storage);
      if (!schunk) { blosc2_free_ctx(cctx); PUSH_ERR("blosc2_filter: Cannot create super-chunk"); }

      if (blosc2_schunk_append_buffer(schunk, *buf, (int32_t)nbytes) < 0) {
        blosc2_schunk_free(schunk); blosc2_free_ctx(cctx);
        PUSH_ERR("blosc2_filter: Cannot append buffer to super-chunk");
      }

      bool needs_free = false;
      uint8_t *tmp_out = NULL;
      status = blosc2_schunk_to_buffer(schunk, &tmp_out, &needs_free);
      
      if (status <= 0) {
          if (needs_free && tmp_out) free(tmp_out);
          blosc2_schunk_free(schunk); 
          blosc2_free_ctx(cctx);
          PUSH_ERR("blosc2_filter: Super-chunk compression failed");
      }

      outbuf = H5allocate_memory((size_t)status, 0);
      memcpy(outbuf, tmp_out, (size_t)status);
      
      if (needs_free && tmp_out) free(tmp_out);
      blosc2_schunk_free(schunk);
      blosc2_free_ctx(cctx);
    }
  } 
  
  /* ----- Decompression Path ----- */
  else {
    /* false prevents double-allocating compressed buffers */
    blosc2_schunk *schunk = blosc2_schunk_from_buffer(*buf, (int64_t)nbytes, false);
    if (!schunk) PUSH_ERR("blosc2_filter: Cannot get super-chunk from buffer");

    /* B2ND Array Decompression */
    if (blosc2_meta_exists(schunk, "b2nd") >= 0 || blosc2_meta_exists(schunk, "caterva") >= 0) {
      b2nd_array_t *array = NULL;

      if (b2nd_from_schunk(schunk, &array) < 0) {
        blosc2_schunk_free(schunk);
        PUSH_ERR("blosc2_filter: Cannot create B2ND array");
      }
      
      int64_t start[BLOSC2_MAX_DIM] = {0};
      int64_t stop[BLOSC2_MAX_DIM] = {0};
      int64_t size = typesize;
      
      for (int i = 0; i < array->ndim; i++) {
        start[i] = 0;
        stop[i] = array->shape[i];
        size *= array->shape[i];
        
        /* Ensure margin chunks correctly parse padding */
        if (ndim >= 0 && array->shape[i] != chunkshape[i]) {
            snprintf(errmsg, sizeof(errmsg), "blosc2_filter: B2ND shape[%d] != chunkshape[%d]", i, i);
            b2nd_free(array); 
            PUSH_ERR(errmsg);
        }
      }

      outbuf = H5allocate_memory((size_t)size, 0);
      if (!outbuf) { 
        b2nd_free(array); 
        PUSH_ERR("blosc2_filter: Cannot allocate decompression buffer"); 
      }

      if (b2nd_get_slice_cbuffer(array, start, stop, outbuf, stop, (int32_t)size) < 0) {
        H5free_memory(outbuf); 
        b2nd_free(array);
        PUSH_ERR("blosc2_filter: Cannot decompress B2ND array");
      }
      
      status = size;
      b2nd_free(array);
      schunk = NULL; /* Sever pointer to prevent double-free since b2nd_free handles it */
    } 
    /* 1D Linear Decompression */
    else {
      uint8_t *chunk = NULL;
      bool needs_free = false;
      int32_t cbytes = blosc2_schunk_get_lazychunk(schunk, 0, &chunk, &needs_free);
      
      if (cbytes < 0) { 
        blosc2_schunk_free(schunk); 
        PUSH_ERR("blosc2_filter: Cannot get chunk from super-chunk"); 
      }

      int32_t exact_bytes;
      blosc2_cbuffer_sizes(chunk, &exact_bytes, NULL, NULL);
      outbuf_size = (size_t)exact_bytes;

      outbuf = H5allocate_memory(outbuf_size, 0);
      if (!outbuf) { 
        if (needs_free && chunk) free(chunk);
        blosc2_schunk_free(schunk);
        PUSH_ERR("blosc2_filter: Cannot allocate decompression buffer"); 
      }

      blosc2_dparams dparams = BLOSC2_DPARAMS_DEFAULTS;
      dparams.schunk = schunk;
      blosc2_context *dctx = blosc2_create_dctx(dparams);
      
      status = blosc2_decompress_ctx(dctx, chunk, cbytes, outbuf, (int32_t)outbuf_size);
      
      blosc2_free_ctx(dctx);
      if (needs_free && chunk) free(chunk);

      if (status <= 0) {
        H5free_memory(outbuf);
        blosc2_schunk_free(schunk);
        PUSH_ERR("blosc2_filter: Cannot decompress chunk into buffer");
      }
    }
    
    if (schunk) blosc2_schunk_free(schunk);
  }

  if (status > 0 && outbuf) {
    H5free_memory(*buf);
    *buf = outbuf;
    *buf_size = (flags & H5Z_FLAG_REVERSE) ? outbuf_size : (size_t)status;
    return (size_t)status;
  }

  if (outbuf) H5free_memory(outbuf);
  return 0;
}

const H5Z_class2_t blosc2_class = { 
  H5Z_CLASS_T_VERS, 
  FILTER_BLOSC2, 
  1, 1, 
  "blosc2", 
  NULL, 
  blosc2_set_local, 
  blosc2_filter_function 
};
