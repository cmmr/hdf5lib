/**
 * @file blosc2_plugin.c
 * @brief Standalone HDF5 Filter Plugin for Blosc2 (Filter ID 32026)
 */

#include <hdf5.h>
#include <blosc2.h>
#include <b2nd.h>
#include <stdlib.h>
#include <string.h>

#define FILTER_BLOSC2 32026
#define FILTER_BLOSC2_VERSION 1
#define DEFAULT_CLEVEL 5
#define DEFAULT_SHUFFLE 1
#define DEFAULT_COMPCODE BLOSC_BLOSCLZ
#define MAX_FILTER_VALUES (8 + BLOSC2_MAX_DIM)

#define B2ND_OPAQUE_NPDTYPE_FORMAT "|V%zd"
#define B2ND_OPAQUE_NPDTYPE_MAXLEN (2 + 20 + 1)

#define PUSH_ERR(...) do { \
    H5Epush(H5E_DEFAULT, __FILE__, __func__, __LINE__, H5E_ERR_CLS, H5E_PLINE, H5E_CANTFILTER, __VA_ARGS__); \
    return 0; \
} while(0)

/* set_local remains the same as your original provided version */
static herr_t blosc2_set_local(hid_t dcpl, hid_t type, hid_t space) {
  int ndim, i;
  herr_t r;
  unsigned int typesize, basetypesize, bufsize, flags;
  hsize_t chunkshape[H5S_MAX_RANK];
  size_t nelements = MAX_FILTER_VALUES;
  unsigned int values[MAX_FILTER_VALUES];

  memset(values, 0, sizeof(values));
  r = H5Pget_filter_by_id(dcpl, FILTER_BLOSC2, &flags, &nelements, values, 0, NULL, NULL);
  if (r < 0) return -1;
  if (nelements < 4) nelements = 4;
  values[0] = FILTER_BLOSC2_VERSION;
  ndim = H5Pget_chunk(dcpl, H5S_MAX_RANK, chunkshape);
  if (ndim < 0) return -1;

  typesize = (unsigned int)H5Tget_size(type);
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
  for (i = 0; i < ndim; i++) bufsize *= (unsigned int)chunkshape[i];
  values[3] = bufsize;

  if (1 < ndim && ndim <= BLOSC2_MAX_DIM) {
    if (nelements < 5) values[4] = DEFAULT_CLEVEL;
    if (nelements < 6) values[5] = DEFAULT_SHUFFLE;
    if (nelements < 7) values[6] = DEFAULT_COMPCODE;
    values[7] = ndim;
    for (int j = 0; j < ndim; j++) values[8 + j] = (unsigned int)(chunkshape[j]);
    nelements = 8 + ndim;
  } 

  return H5Pmodify_filter(dcpl, FILTER_BLOSC2, flags, nelements, values);
}

/* Helper functions for blocksize and b2nd shape remain identical */
static int32_t compute_blosc2_blocksize(int32_t chunksize, int32_t typesize, int clevel, int compcode) {
  uint8_t data_dest[BLOSC2_MAX_OVERHEAD];
  blosc2_cparams cparams = BLOSC2_CPARAMS_DEFAULTS;
  cparams.compcode = (compcode < 0) ? DEFAULT_COMPCODE : compcode;
  cparams.clevel = clevel;
  cparams.typesize = typesize;
  if (blosc2_chunk_zeros(cparams, chunksize, data_dest, BLOSC2_MAX_OVERHEAD) < 0) return -1;
  int32_t blocksize = -1;
  blosc2_cbuffer_sizes(data_dest, NULL, NULL, &blocksize);
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
        if (nitems_new * 2 <= nitems) { nitems_new *= 2; dims_block[i] *= 2; }
      } else if (dims_block[i] < dims_chunk[i]) {
        size_t newitems_ext = (nitems_new / dims_block[i]) * dims_chunk[i];
        if (newitems_ext <= nitems) { nitems_new = newitems_ext; dims_block[i] = dims_chunk[i]; }
      }
    }
    if (nitems_new == nitems_prev) break; 
  }
  return (int32_t)(nitems_new * type_size);
}

static size_t blosc2_filter_function(
    unsigned int flags, size_t cd_nelmts, const unsigned int cd_values[], 
    size_t nbytes, size_t *buf_size, void **buf) {

  if (nbytes == 0) return 0;
  void *outbuf = NULL;
  int64_t status = 0;
  size_t blocksize, typesize, outbuf_size;
  
  if (cd_nelmts < 4) PUSH_ERR("blosc2_filter: Too few parameters");
  blocksize = cd_values[1]; 
  typesize = cd_values[2]; 
  outbuf_size = cd_values[3]; 

  blosc2_init();

  if (!(flags & H5Z_FLAG_REVERSE)) {
    int clevel = (cd_nelmts >= 5) ? cd_values[4] : DEFAULT_CLEVEL;
    int doshuffle = (cd_nelmts >= 6) ? cd_values[5] : DEFAULT_SHUFFLE;
    int compcode = (cd_nelmts >= 7) ? cd_values[6] : DEFAULT_COMPCODE;

    blosc2_cparams cparams = BLOSC2_CPARAMS_DEFAULTS;
    cparams.compcode = compcode;
    cparams.typesize = (int32_t)typesize;
    cparams.filters[BLOSC_LAST_FILTER] = doshuffle;
    cparams.clevel = clevel;
    blosc2_storage storage = {.cparams = &cparams, .contiguous = false};

    if (cd_nelmts >= 8 && cd_values[7] > 1) {
      int ndim = cd_values[7];
      int32_t chunkshape[BLOSC2_MAX_DIM], blockdims[BLOSC2_MAX_DIM];
      int64_t chunkshape_l[BLOSC2_MAX_DIM];
      for (int i = 0; i < ndim; i++) {
          chunkshape[i] = (int32_t)cd_values[8+i];
          chunkshape_l[i] = chunkshape[i];
      }
      
      if (blocksize == 0) blocksize = compute_blosc2_blocksize((int32_t)outbuf_size, (int32_t)typesize, clevel, compcode);
      cparams.blocksize = compute_b2nd_block_shape(blocksize, typesize, ndim, chunkshape, blockdims);
      
      char dtype[B2ND_OPAQUE_NPDTYPE_MAXLEN];
      snprintf(dtype, sizeof(dtype), B2ND_OPAQUE_NPDTYPE_FORMAT, typesize);
      b2nd_context_t *ctx = b2nd_create_ctx(&storage, ndim, chunkshape_l, chunkshape, blockdims, dtype, DTYPE_NUMPY_FORMAT, NULL, 0);
      
      b2nd_array_t *array = NULL;
      if (b2nd_from_cbuffer(ctx, &array, *buf, (int32_t)nbytes) < 0) { b2nd_free_ctx(ctx); PUSH_ERR("B2ND compress failed"); }

      bool needs_free;
      uint8_t *tmp_out = NULL;
      if (b2nd_to_cframe(array, &tmp_out, &status, &needs_free) < 0) { b2nd_free(array); b2nd_free_ctx(ctx); PUSH_ERR("B2ND to cframe failed"); }

      /* CHECK FOR INCOMPRESSIBLE DATA */
      if (status >= (int64_t)nbytes) {
          if (needs_free) free(tmp_out);
          b2nd_free(array); b2nd_free_ctx(ctx);
          blosc2_destroy();
          return 0;
      }

      outbuf = H5allocate_memory((size_t)status, 0);
      if (outbuf) memcpy(outbuf, tmp_out, (size_t)status);
      if (needs_free) free(tmp_out);
      b2nd_free(array); b2nd_free_ctx(ctx);
    } else {
      cparams.blocksize = (int32_t)blocksize;
      blosc2_schunk *schunk = blosc2_schunk_new(&storage);
      if (blosc2_schunk_append_buffer(schunk, *buf, (int32_t)nbytes) < 0) { blosc2_schunk_free(schunk); PUSH_ERR("Schunk append failed"); }

      bool needs_free;
      uint8_t *tmp_out = NULL;
      status = blosc2_schunk_to_buffer(schunk, &tmp_out, &needs_free);
      
      /* CHECK FOR INCOMPRESSIBLE DATA */
      if (status <= 0 || status >= (int64_t)nbytes) {
          if (needs_free) free(tmp_out);
          blosc2_schunk_free(schunk);
          blosc2_destroy();
          return 0;
      }

      outbuf = H5allocate_memory((size_t)status, 0);
      if (outbuf) memcpy(outbuf, tmp_out, (size_t)status);
      if (needs_free) free(tmp_out);
      blosc2_schunk_free(schunk);
    }
  } else {
    /* Decompression Path: Ensure schunk and chunks are always freed */
    blosc2_schunk *schunk = blosc2_schunk_from_buffer(*buf, (int64_t)nbytes, false);
    if (!schunk) PUSH_ERR("blosc2_filter: Buffer is not a valid Blosc2 frame");

    if (blosc2_meta_exists(schunk, "b2nd") >= 0 || blosc2_meta_exists(schunk, "caterva") >= 0) {
      b2nd_array_t *array = NULL;
      b2nd_from_schunk(schunk, &array);
      int64_t start[BLOSC2_MAX_DIM], stop[BLOSC2_MAX_DIM];
      size_t total_size = typesize;
      for (int i = 0; i < array->ndim; i++) { start[i] = 0; stop[i] = array->shape[i]; total_size *= array->shape[i]; }
      
      outbuf = H5allocate_memory(total_size, 0);
      if (outbuf && b2nd_get_slice_cbuffer(array, start, stop, outbuf, stop, (int32_t)total_size) >= 0) {
          status = (int64_t)total_size;
          outbuf_size = total_size;
      }
      b2nd_free(array);
    } else {
      uint8_t *chunk;
      bool needs_free;
      int32_t cbytes = blosc2_schunk_get_lazychunk(schunk, 0, &chunk, &needs_free);
      int32_t exact_bytes;
      blosc2_cbuffer_sizes(chunk, &exact_bytes, NULL, NULL);
      outbuf = H5allocate_memory(exact_bytes, 0);
      if (outbuf && blosc2_decompress(chunk, cbytes, outbuf, exact_bytes) >= 0) {
          status = exact_bytes;
          outbuf_size = (size_t)exact_bytes;
      }
      if (needs_free) free(chunk);
    }
    blosc2_schunk_free(schunk);
  }

  if (status > 0 && outbuf) {
    H5free_memory(*buf);
    *buf = outbuf;
    *buf_size = outbuf_size;
    blosc2_destroy();
    return (size_t)status;
  }
  
  if (outbuf) H5free_memory(outbuf);
  blosc2_destroy();
  return 0;
}

const H5Z_class2_t blosc2_class = { H5Z_CLASS_T_VERS, FILTER_BLOSC2, 1, 1, "blosc2", NULL, blosc2_set_local, blosc2_filter_function };
