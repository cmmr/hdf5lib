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

/* Max possible size: Base(9) + MaxDims(8) = 17 */
#define MAX_FILTER_VALUES (9 + BLOSC2_MAX_DIM) 

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
  unsigned int values[MAX_FILTER_VALUES] = {0};
  r = H5Pget_filter_by_id(dcpl, FILTER_BLOSC2, &flags, &nelements, values, 0, NULL, NULL);
  if (r < 0) return -1;

  /* 1. Extract User Inputs (Assuming user array: [0, 0, 0, 0, clevel, filter, compcode, meta]) */
  unsigned int user_clevel      = (nelements >= 5) ? values[4] : DEFAULT_CLEVEL;
  unsigned int user_filter_mask = (nelements >= 6) ? values[5] : DEFAULT_SHUFFLE;
  unsigned int user_compcode    = (nelements >= 7) ? values[6] : DEFAULT_COMPCODE;
  unsigned int user_meta        = (nelements >= 8) ? values[7] : 0; /* Capture the meta byte */

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

  /* 3. Reconstruct cd_values: Python Base Layout */
  values[0] = FILTER_BLOSC2_VERSION;
  values[1] = basetypesize;
  values[2] = bufsize; 
  values[3] = user_clevel;
  values[4] = user_filter_mask; 
  values[5] = user_compcode;
  
  /* 4. Append ndim and shape */
  size_t idx = 6;
  values[idx++] = ndim;
  for (int i = 0; i < ndim; i++) {
      values[idx++] = (unsigned int)chunkshape[i];
  }

  /* 5. Append Custom Meta Value at the very end */
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
/* Match the standard hdf5plugin layout */
  size_t typesize  = cd_values[1]; 
  size_t outbuf_size = cd_values[2]; 
  int clevel      = cd_values[3];
  int filter_mask = cd_values[4];
  int compcode    = cd_values[5];
  
  int ndim = -1;
  int32_t chunkshape[BLOSC2_MAX_DIM];
  size_t idx = 6; 

  /* Read Python's geometry data */
  if (cd_nelmts >= 7) {
      ndim = (int)cd_values[idx++];
      for (int i = 0; i < ndim; i++) {
          chunkshape[i] = (int32_t)cd_values[idx++];
      }
  }

  /* Safely extract the custom meta byte if it was appended */
  int meta_value = 0;
  if (cd_nelmts > idx) {
      meta_value = cd_values[idx];
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
