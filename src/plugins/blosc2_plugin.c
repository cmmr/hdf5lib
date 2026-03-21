/**
 * @file blosc2_plugin.c
 * @brief Standalone HDF5 Filter Plugin for Blosc2 (Filter ID 32026)
 * Supports Bitmask Pre-Filter Pipelines while maintaining 100% 
 * forward and backward compatibility with h5py and community plugins.
 * * Includes ZFP Safety Overrides to prevent bit-corruption from pre-filters.
 */

#include <hdf5.h>
#include <blosc2.h>
#include <b2nd.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>

#define FILTER_BLOSC2 32026
#define FILTER_BLOSC2_VERSION 2
#define DEFAULT_CLEVEL 5
#define DEFAULT_SHUFFLE 1
#define DEFAULT_COMPCODE BLOSC_BLOSCLZ

/* Max possible size: Base(8) + MaxDims(8) + Meta(1) = 17 */
#define MAX_FILTER_VALUES (8 + BLOSC2_MAX_DIM + 1) 

#ifdef WORDS_BIGENDIAN
  #define B2ND_ENDIAN_PREFIX ">"
#else
  #define B2ND_ENDIAN_PREFIX "<"
#endif

#define B2ND_OPAQUE_NPDTYPE_FORMAT "|V%zd"
#define B2ND_OPAQUE_NPDTYPE_MAXLEN (2 + 20 + 1)

#define PUSH_ERR(...) do { \
    H5Epush(H5E_DEFAULT, __FILE__, __func__, __LINE__, H5E_ERR_CLS, H5E_PLINE, H5E_CANTFILTER, __VA_ARGS__); \
    return 0; \
} while(0)

/* =========================================================================
 * SET_LOCAL CALLBACK (METADATA INJECTION & PARSING)
 * ========================================================================= */
static herr_t blosc2_set_local(hid_t dcpl, hid_t type, hid_t space) {
  int ndim;
  herr_t r;
  unsigned int typesize, basetypesize, bufsize;
  hsize_t chunkshape[H5S_MAX_RANK];
  unsigned int flags;
  
  size_t nelements = MAX_FILTER_VALUES;
  unsigned int values[MAX_FILTER_VALUES];
  memset(values, 0, sizeof(values));

  r = H5Pget_filter_by_id(dcpl, FILTER_BLOSC2, &flags, &nelements, values, 0, NULL, NULL);
  if (r < 0) return -1;

  /* 1. Extract User Inputs (Assuming user array: [0,0,0,0, clevel, filter, compcode, meta]) */
  unsigned int user_clevel      = (nelements >= 5) ? values[4] : DEFAULT_CLEVEL;
  unsigned int user_filter_mask = (nelements >= 6) ? values[5] : DEFAULT_SHUFFLE;
  unsigned int user_compcode    = (nelements >= 7) ? values[6] : DEFAULT_COMPCODE;
  unsigned int user_meta        = (nelements >= 8) ? values[7] : 0;

  /* 2. Calculate Data Geometry */
  ndim = H5Pget_chunk(dcpl, H5S_MAX_RANK, chunkshape);
  if (ndim < 0) return -1;

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

  bufsize = typesize;
  for (int i = 0; i < ndim; i++) bufsize *= (unsigned int)chunkshape[i];

  /* 3. Reconstruct cd_values: Strict Python Base Layout */
  values[0] = FILTER_BLOSC2_VERSION;
  values[1] = 0; /* Blocksize: 0 tells Python/Blosc to calculate it automatically */
  values[2] = basetypesize;
  values[3] = bufsize; 
  values[4] = user_clevel;
  values[5] = user_filter_mask; 
  values[6] = user_compcode;
  
  /* 4. Append ndim and shape starting precisely at index 7 */
  size_t idx = 7;
  values[idx++] = ndim;
  for (int i = 0; i < ndim; i++) {
      values[idx++] = (unsigned int)chunkshape[i];
  }

  /* 5. Append Custom Meta Value out of Python's reach */
  values[idx++] = user_meta;

  nelements = idx;
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

  if (cd_nelmts < 7) PUSH_ERR("blosc2_filter: Filter parameters corrupted");

  /* Match the strict Python layout indices exactly */
  size_t blocksize   = cd_values[1]; 
  size_t typesize    = cd_values[2]; 
  size_t outbuf_size = cd_values[3]; 
  int clevel         = cd_values[4];
  int filter_mask    = cd_values[5];
  int compcode       = cd_values[6];
  
  int ndim = -1;
  int32_t chunkshape[BLOSC2_MAX_DIM];
  size_t idx = 7; 

  /* Only extract appended C geometry if the writer included it */
  if (cd_nelmts >= 8) {
      ndim = (int)cd_values[idx++];
      for (int i = 0; i < ndim; i++) {
          chunkshape[i] = (int32_t)cd_values[idx++];
      }
  }

  /* Extract custom meta byte if it was appended */
  int meta_value = 0;
  if (cd_nelmts > idx) {
      meta_value = cd_values[idx];
  }

  /* ----- Compression Path ----- */
  if (!(flags & H5Z_FLAG_REVERSE)) {

    blosc2_cparams cparams = BLOSC2_CPARAMS_DEFAULTS;
    cparams.compcode = compcode;
    cparams.typesize = (int32_t)typesize;
    cparams.clevel   = clevel;

    if (compcode == 33 || compcode == 34 || compcode == 35) {
        cparams.compcode_meta = meta_value;
    } else {
        cparams.compcode_meta = 0;
    }

    for (int i = 0; i < BLOSC2_MAX_FILTERS; i++) {
        cparams.filters[i] = 0; 
        cparams.filters_meta[i] = 0;
    }

    /* ZFP strictly bypasses Bitmask pre-filters to prevent mantissa corruption */
    if (compcode != 33 && compcode != 34 && compcode != 35) {
        int f_idx = 0;
        
        if (filter_mask & 8) {
            cparams.filters[f_idx] = BLOSC_TRUNC_PREC;
            cparams.filters_meta[f_idx] = meta_value; 
            f_idx++;
        }
        
        if (filter_mask & 4) {
            cparams.filters[f_idx] = BLOSC_DELTA;
            f_idx++;
        }
        
        if (filter_mask & 2) {
            cparams.filters[f_idx] = BLOSC_BITSHUFFLE;
            f_idx++;
        } else if (filter_mask & 1) {
            cparams.filters[f_idx] = BLOSC_SHUFFLE;
            f_idx++;
        }
    }

    blosc2_storage storage = {.cparams = &cparams, .contiguous = false};

    /* Multi-Dimensional (B2ND) Chunking */
    if (ndim > 1 || (compcode >= 33 && compcode <= 35)) {
      b2nd_context_t *ctx = NULL;
      b2nd_array_t *array = NULL;

      if (blocksize == 0) {
        int32_t sugg = compute_blosc2_blocksize((int32_t)outbuf_size, (int32_t)typesize, cparams.clevel, compcode);
        if (sugg < 0) PUSH_ERR("blosc2_filter: Failed to compute suggested blocksize");
        blocksize = sugg;
      }
      
      int32_t blockdims[BLOSC2_MAX_DIM];
      cparams.blocksize = compute_b2nd_block_shape(blocksize, typesize, ndim, chunkshape, blockdims);

      int64_t chunkshape_l[BLOSC2_MAX_DIM];
      for (int i = 0; i < ndim; i++) chunkshape_l[i] = chunkshape[i];

      char dtype[B2ND_OPAQUE_NPDTYPE_MAXLEN];

      if (compcode >= 33 && compcode <= 35) {
          snprintf(dtype, sizeof(dtype), "%sf%u", B2ND_ENDIAN_PREFIX, (unsigned int)typesize);
      } else {
          snprintf(dtype, sizeof(dtype), B2ND_OPAQUE_NPDTYPE_FORMAT, (size_t)typesize);
      }
      
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
      
      /* Trust the B2ND header entirely. Ignore cd_values chunkshape perfectly. */
      int64_t size = schunk->typesize; 
      for (int i = 0; i < array->ndim; i++) {
        start[i] = 0;
        stop[i] = array->shape[i];
        size *= array->shape[i];
      }

      outbuf_size = (size_t)size;

      outbuf = H5allocate_memory(outbuf_size, 0);
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
      schunk = NULL;
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
