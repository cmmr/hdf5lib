#include <R.h>
#include <Rinternals.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <hdf5.h>
#include <hdf5_hl.h>
#include <hdf5lib.h>

#define H5Z_FILTER_BZIP2     307
#define H5Z_FILTER_SZIP      4
#define H5Z_FILTER_LZF       32000
#define H5Z_FILTER_BLOSC     32001
#define H5Z_FILTER_SNAPPY    32003
#define H5Z_FILTER_LZ4       32004
#define H5Z_FILTER_BSHUF     32008
#define H5Z_FILTER_ZFP       32013
#define H5Z_FILTER_ZSTD      32015
#define H5Z_FILTER_BITGROOM  32022
#define H5Z_FILTER_GRANULARBR 32023
#define H5Z_FILTER_SZ3       32024
#define H5Z_FILTER_BITROUND  32032
#define H5Z_FILTER_BLOSC2    32026

/* Blosc2's cd_values[0]: tag 0xB2, layout 1, the pre-filter bitmask (1
   shuffle, 2 bitshuffle, 4 delta, 8 trunc-prec), the meta value, and the
   reference filter's revision, 1 */
#define B2_SETTINGS(mask, meta) ((0xB2u << 24) | (1u << 20) | (((mask) & 0xFu) << 16) | \
                                 (((meta) & 0xFFu) << 8) | 0x01u)

/* --- Test Mask Architecture --- */
#define TEST_FLT 1   /* Standard Floating Point Data */
#define TEST_CMP 2   /* Compound Datatypes */
#define TEST_ARR 4   /* Array Datatypes */
#define TEST_ZRO 8   /* Highly Compressible (Zeros) -> Tests Buffer Resizes */
#define TEST_RND 16  /* Incompressible (Random) -> Tests Fallback mechanisms */
#define TEST_UNC 32  /* Unchunked/Contiguous layout */
#define TEST_EMP 64  /* Empty dataset -> Tests nbytes == 0 chunk logic */
#define TEST_INT 128 /* Standard Integer Data */
#define TEST_2D8 256 /* 2-D chunked bytes -> Tests the multidimensional path */
#define TEST_NAN 512 /* NaN / Inf in float data -> Tests the ZFP non-finite guard */
#define TEST_THR 1024 /* Multi-block chunks, 1 and 2 threads -> Tests Blosc context reuse */

#define TEST_ALL          (TEST_FLT | TEST_INT | TEST_CMP | TEST_ARR | TEST_ZRO | TEST_RND | TEST_UNC | TEST_EMP | TEST_2D8)
/* Filters like standalone ZFP and SZIP officially support atomic numerical arrays (Float & Int) */
#define TEST_NUMERIC_ONLY (TEST_FLT | TEST_INT | TEST_ZRO | TEST_UNC | TEST_EMP)
/* Blosc2's internal ZFP codec hard-fails on integers; strictly limit it to float paths */
#define TEST_FLOAT_ONLY   (TEST_FLT | TEST_ZRO | TEST_EMP)

/* NDLZ is a 2-D pattern codec: it compresses images far better than it
   compresses numeric arrays, and readily expands the latter, so exercise it on
   shapes it is meant for plus the layouts that reach it through a byte-image
   remap (1-D, multi-byte items) rather than on the ratio-checking tests. */
#define TEST_ND_ONLY      (TEST_CMP | TEST_ARR | TEST_RND | TEST_2D8)

/* --- Verbose Error Macro --- */
#define CHECK(expr, step_msg) do { \
  if ((expr) < 0) { \
    Rprintf("\n      -> %s failed", step_msg); \
    goto cleanup; \
  } \
} while(0)

/* --- Checked Dataset Close --- */
/* H5Dclose() flushes the chunk cache, so a compression failure on a chunk that
   is still dirty surfaces here rather than at H5Dwrite() time.  HDF5 drops the
   chunk when the pipeline fails, so an unchecked close turns a broken filter
   into silent data loss. */
#define CLOSE_DSET(dset) do { \
  if ((dset) >= 0 && H5Dclose(dset) < 0) { \
    Rprintf("\n      -> H5Dclose failed (chunk flush/compression error)"); \
    ret = -1; \
  } \
} while(0)

/* --- Close and Reopen a Dataset --- */
/* A read through the handle that wrote the data is answered from the chunk
   cache, so the filter never decompresses anything.  Reading through a fresh
   handle makes every chunk go through the pipeline both ways. */
static hid_t reopen_dset(hid_t fid, hid_t dset, const char *name) {
  if (H5Dclose(dset) < 0) {
    Rprintf("\n      -> H5Dclose failed (chunk flush/compression error)");
    return -1;
  }
  return H5Dopen2(fid, name, H5P_DEFAULT);
}

/* --- Filter Configuration Architecture --- */
typedef struct {
  const char* name;
  H5Z_filter_t id;
  size_t       nelmts;
  unsigned int cd_values[12];
  int          test_mask;
} FilterConfig;

/* Helper to apply filter natively */
static herr_t apply_filter(hid_t plist, const FilterConfig* cfg) {
  if (cfg->id == H5Z_FILTER_DEFLATE) {
    return H5Pset_deflate(plist, cfg->cd_values[0]);
  } else if (cfg->id == H5Z_FILTER_SZIP) {
    return H5Pset_szip(plist, cfg->cd_values[0], cfg->cd_values[1]);
  } else if (cfg->id == H5Z_FILTER_ZFP) {
    unsigned int mode = cfg->cd_values[0];
    if (mode == 1) return H5Pset_zfp_rate(plist, (double)cfg->cd_values[2]);
    if (mode == 2) return H5Pset_zfp_precision(plist, cfg->cd_values[2]);
    if (mode == 3) {
      double acc = 1.0;
      for (unsigned int i = 0; i < cfg->cd_values[2]; i++) acc /= 10.0;
      return H5Pset_zfp_accuracy(plist, acc);
    }
    if (mode == 4) return H5Pset_zfp_expert(plist, cfg->cd_values[2], cfg->cd_values[3], cfg->cd_values[4], (int)cfg->cd_values[5]);
    if (mode == 5) return H5Pset_zfp_reversible(plist);
    return -1;
  } else {
    return H5Pset_filter(plist, cfg->id, H5Z_FLAG_MANDATORY, cfg->nelmts, cfg->cd_values);
  }
}

/* =========================================================================
 * CODEC INSPECTOR
 * ========================================================================= */

/* Reads the raw chunk from disk to verify the correct compression flag was used */
static int verify_blosc_compcode(hid_t dset, const FilterConfig* cfg) {
    hsize_t offset[1] = {0}; 
    hsize_t chunk_bytes;
    uint32_t filter_mask = 0;

    if (H5Dget_chunk_info(dset, H5P_DEFAULT, 0, offset, (unsigned *)&filter_mask, NULL, &chunk_bytes) < 0) {
        Rprintf("\n      -> Codec Inspector: Failed to get chunk info.");
        return -1;
    }

    unsigned char *raw_chunk = malloc((size_t)chunk_bytes);
    if (!raw_chunk) return -1;

    size_t chunk_buf_size = (size_t)chunk_bytes;
    if (H5Dread_chunk(dset, H5P_DEFAULT, offset, &filter_mask, raw_chunk, &chunk_buf_size) < 0) {
        Rprintf("\n      -> Codec Inspector: Failed to read raw chunk.");
        free(raw_chunk);
        return -1;
    }

    int ret = 0;
    
    if (cfg->id == H5Z_FILTER_BLOSC2) {
        /* Verify Blosc2 frame magic header */
        if (!(raw_chunk[2] == 'b' && raw_chunk[3] == '2' && raw_chunk[4] == 'f')) {
            Rprintf("\n      -> Disk Inspection: Missing expected Blosc2 frame header (b2frame)");
            ret = -1;
        }
    } else if (cfg->id == H5Z_FILTER_BLOSC) {
        /* Verify Blosc1 Compformat bits */
        unsigned char flags = raw_chunk[2];
        unsigned char compformat = (flags >> 5) & 7;
        unsigned char requested_codec = (unsigned char)cfg->cd_values[6];
        
        /* Calculate the expected disk ID. 
           LZ4 (1) and LZ4HC (2) share the exact same format ID (1) on disk.
           This shifts all subsequent compressor IDs down by 1 in the binary header. */
        unsigned char expected_disk_id = requested_codec;
        if (requested_codec >= 2) {
            expected_disk_id = requested_codec - 1;
        }
        
        if (compformat != expected_disk_id) {
            Rprintf("\n      -> Disk Inspection: Expected Blosc1 Codec %d (Disk ID %d), got %d", 
                    requested_codec, expected_disk_id, compformat);
            ret = -1;
        }
    }

    free(raw_chunk);
    return ret;
}

/* =========================================================================
 * DATATYPE TEST RUNNERS
 * ========================================================================= */

/* 1. Float Data Test */
static int test_float_data(hid_t fid, const FilterConfig* cfg) {
  #define FLT_SIZE 10000 /* 40KB - Ensures metadata overhead doesn't skew % checks */
  float *data = malloc(FLT_SIZE * sizeof(float));
  float *data_out = malloc(FLT_SIZE * sizeof(float));
  hsize_t dims[1] = {FLT_SIZE}, chunkdims[1] = {10000};
  hid_t sid = -1, plist = -1, dset = -1;
  int ret = -1;

  if (!data || !data_out) goto cleanup;

  for(int i=0; i<FLT_SIZE; i++) { 
    data[i] = (float)(i % 100); /* Highly compressible repeating sequence */
    data_out[i] = -1.0f; 
  }

  CHECK(sid = H5Screate_simple(1, dims, NULL), "H5Screate_simple");
  CHECK(plist = H5Pcreate(H5P_DATASET_CREATE), "H5Pcreate");
  CHECK(H5Pset_chunk(plist, 1, chunkdims), "H5Pset_chunk");
  CHECK(apply_filter(plist, cfg), "apply_filter");

  char dset_name[128];
  snprintf(dset_name, sizeof(dset_name), "/float_%s", cfg->name);
  CHECK(dset = H5Dcreate2(fid, dset_name, H5T_NATIVE_FLOAT, sid, H5P_DEFAULT, plist, H5P_DEFAULT), "H5Dcreate2");
  CHECK(H5Dwrite(dset, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, H5P_DEFAULT, data), "H5Dwrite");

  /* Force execution of the pipeline to disk */
  CHECK(H5Dflush(dset), "H5Dflush (Compression Pipeline Crashed!)"); 

  /* Validate Chunk Metadata */
  if (cfg->id == H5Z_FILTER_BLOSC2 || cfg->id == H5Z_FILTER_BLOSC) {
      if (verify_blosc_compcode(dset, cfg) < 0) goto cleanup;
  }

  hsize_t allocated_size = H5Dget_storage_size(dset);
  hsize_t uncompressed_size = FLT_SIZE * sizeof(float);

  int skip_size_check = 0;
  if (cfg->id == H5Z_FILTER_BSHUF && cfg->cd_values[1] == 0) skip_size_check = 1;

  if (!skip_size_check && allocated_size >= (uncompressed_size * 0.9)) {
      Rprintf("\n      -> Silent Failure: Float data was not compressed (Allocated: %llu, Raw: %llu)", 
              (unsigned long long)allocated_size, (unsigned long long)uncompressed_size);
      goto cleanup;
  }

  CHECK(dset = reopen_dset(fid, dset, dset_name), "reopen");
  CHECK(H5Dread(dset, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, H5P_DEFAULT, data_out), "H5Dread");

  for(int i=0; i<FLT_SIZE; i++) {
    /* Tolerance of 1.0f easily supports lossy rates (e.g. ZFP rate=16) */
    float diff = data[i] - data_out[i];
    if(diff > 1.0f || diff < -1.0f) {
      Rprintf("\n      -> Float mismatch at index %d (in: %f, out: %f)", i, data[i], data_out[i]);
      goto cleanup;
    }
  }
  ret = 0;

cleanup:
  if (data) free(data);
  if (data_out) free(data_out);
  CLOSE_DSET(dset);
  if (plist >= 0) H5Pclose(plist);
  if (sid >= 0) H5Sclose(sid);
  return ret;
}

/* 2. Integer Data Test */
static int test_int_data(hid_t fid, const FilterConfig* cfg) {
  #define INT_SIZE 10000 
  int *data = malloc(INT_SIZE * sizeof(int));
  int *data_out = malloc(INT_SIZE * sizeof(int));
  hsize_t dims[1] = {INT_SIZE}, chunkdims[1] = {10000};
  hid_t sid = -1, plist = -1, dset = -1;
  int ret = -1;

  if (!data || !data_out) goto cleanup;

  for(int i=0; i<INT_SIZE; i++) { 
    data[i] = i % 100; /* Compresses heavily */
    data_out[i] = -1; 
  }

  CHECK(sid = H5Screate_simple(1, dims, NULL), "H5Screate_simple");
  CHECK(plist = H5Pcreate(H5P_DATASET_CREATE), "H5Pcreate");
  CHECK(H5Pset_chunk(plist, 1, chunkdims), "H5Pset_chunk");
  CHECK(apply_filter(plist, cfg), "apply_filter");

  char dset_name[128];
  snprintf(dset_name, sizeof(dset_name), "/int_%s", cfg->name);
  CHECK(dset = H5Dcreate2(fid, dset_name, H5T_NATIVE_INT, sid, H5P_DEFAULT, plist, H5P_DEFAULT), "H5Dcreate2");
  CHECK(H5Dwrite(dset, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, data), "H5Dwrite");

  CHECK(H5Dflush(dset), "H5Dflush"); 

  if (cfg->id == H5Z_FILTER_BLOSC2 || cfg->id == H5Z_FILTER_BLOSC) {
      if (verify_blosc_compcode(dset, cfg) < 0) goto cleanup;
  }

  hsize_t allocated_size = H5Dget_storage_size(dset);
  hsize_t uncompressed_size = INT_SIZE * sizeof(int);

  int skip_size_check = 0;
  if (cfg->id == H5Z_FILTER_BSHUF && cfg->cd_values[1] == 0) skip_size_check = 1;

  if (!skip_size_check && allocated_size >= (uncompressed_size * 0.9)) {
      Rprintf("\n      -> Silent Failure: Integer data was not compressed (Allocated: %llu, Raw: %llu)", 
              (unsigned long long)allocated_size, (unsigned long long)uncompressed_size);
      goto cleanup;
  }

  CHECK(dset = reopen_dset(fid, dset, dset_name), "reopen");
  CHECK(H5Dread(dset, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, data_out), "H5Dread");

  for(int i=0; i<INT_SIZE; i++) {
    int diff = data[i] - data_out[i];
    if(diff > 5 || diff < -5) {
      Rprintf("\n      -> Int mismatch at index %d (in: %d, out: %d)", i, data[i], data_out[i]);
      goto cleanup;
    }
  }
  ret = 0;

cleanup:
  if (data) free(data);
  if (data_out) free(data_out);
  CLOSE_DSET(dset);
  if (plist >= 0) H5Pclose(plist);
  if (sid >= 0) H5Sclose(sid);
  return ret;
}

/* 3. Compound Data Test */
static int test_compound_data(hid_t fid, const FilterConfig* cfg) {
  #define CMP_SIZE 1000
  #define STRUCT_SIZE 260
  unsigned char *data = NULL, *data_out = NULL;
  hsize_t dims[1] = {CMP_SIZE}, chunkdims[1] = {100};
  hid_t sid = -1, plist = -1, dtype = -1, dset = -1;
  int ret = -1;

  data = malloc(CMP_SIZE * STRUCT_SIZE);
  data_out = malloc(CMP_SIZE * STRUCT_SIZE);
  if (!data || !data_out) goto cleanup;
  for(int i=0; i < CMP_SIZE * STRUCT_SIZE; i++) data[i] = (unsigned char)(i % 256);

  CHECK(sid = H5Screate_simple(1, dims, NULL), "H5Screate_simple");
  CHECK(plist = H5Pcreate(H5P_DATASET_CREATE), "H5Pcreate");
  CHECK(H5Pset_chunk(plist, 1, chunkdims), "H5Pset_chunk");
  CHECK(apply_filter(plist, cfg), "apply_filter");

  CHECK(dtype = H5Tcreate(H5T_COMPOUND, STRUCT_SIZE), "H5Tcreate");
  for (int i=0; i<STRUCT_SIZE; i++) {
    char field_name[32];
    snprintf(field_name, sizeof(field_name), "field_%d", i);
    CHECK(H5Tinsert(dtype, field_name, i, H5T_NATIVE_UCHAR), "H5Tinsert");
  }

  char dset_name[128];
  snprintf(dset_name, sizeof(dset_name), "/compound_%s", cfg->name);
  CHECK(dset = H5Dcreate2(fid, dset_name, dtype, sid, H5P_DEFAULT, plist, H5P_DEFAULT), "H5Dcreate2");
  CHECK(H5Dwrite(dset, dtype, H5S_ALL, H5S_ALL, H5P_DEFAULT, data), "H5Dwrite");
  CHECK(dset = reopen_dset(fid, dset, dset_name), "reopen");
  CHECK(H5Dread(dset, dtype, H5S_ALL, H5S_ALL, H5P_DEFAULT, data_out), "H5Dread");

  for(int i=0; i < CMP_SIZE * STRUCT_SIZE; i++) {
    if(data[i] != data_out[i]) {
      Rprintf("\n      -> Compound mismatch at index %d", i);
      goto cleanup;
    }
  }
  ret = 0;

cleanup:
  if (data) free(data);
  if (data_out) free(data_out);
  CLOSE_DSET(dset);
  if (dtype >= 0) H5Tclose(dtype);
  if (plist >= 0) H5Pclose(plist);
  if (sid >= 0) H5Sclose(sid);
  return ret;
}

/* 4. Array Data Test */
static int test_array_data(hid_t fid, const FilterConfig* cfg) {
  #define ARR_SIZE 1000
  #define SUB_X 10
  #define SUB_Y 10
  #define TOTAL_ELTS (ARR_SIZE * SUB_X * SUB_Y)
  
  float *data = NULL, *data_out = NULL;
  hsize_t dims[1] = {ARR_SIZE}, chunkdims[1] = {100};
  hsize_t type_shape[2] = {SUB_X, SUB_Y};
  hid_t sid = -1, plist = -1, dtype = -1, dset = -1;
  int ret = -1;

  data = malloc(TOTAL_ELTS * sizeof(float));
  data_out = malloc(TOTAL_ELTS * sizeof(float));
  if (!data || !data_out) goto cleanup;
  for(int i=0; i < TOTAL_ELTS; i++) { data[i] = (float)i; data_out[i] = -1.0; }

  CHECK(sid = H5Screate_simple(1, dims, NULL), "H5Screate_simple");
  CHECK(plist = H5Pcreate(H5P_DATASET_CREATE), "H5Pcreate");
  CHECK(H5Pset_chunk(plist, 1, chunkdims), "H5Pset_chunk");
  CHECK(apply_filter(plist, cfg), "apply_filter");

  CHECK(dtype = H5Tarray_create(H5T_NATIVE_FLOAT, 2, type_shape), "H5Tarray_create");

  char dset_name[128];
  snprintf(dset_name, sizeof(dset_name), "/array_%s", cfg->name);
  CHECK(dset = H5Dcreate2(fid, dset_name, dtype, sid, H5P_DEFAULT, plist, H5P_DEFAULT), "H5Dcreate2");
  CHECK(H5Dwrite(dset, dtype, H5S_ALL, H5S_ALL, H5P_DEFAULT, data), "H5Dwrite");
  CHECK(dset = reopen_dset(fid, dset, dset_name), "reopen");
  CHECK(H5Dread(dset, dtype, H5S_ALL, H5S_ALL, H5P_DEFAULT, data_out), "H5Dread");

  for(int i=0; i < TOTAL_ELTS; i++) {
    if(data[i] - data_out[i] > 0.1 || data_out[i] - data[i] > 0.1) {
      Rprintf("\n      -> Array mismatch at index %d", i);
      goto cleanup;
    }
  }
  ret = 0;

cleanup:
  if (data) free(data);
  if (data_out) free(data_out);
  CLOSE_DSET(dset);
  if (dtype >= 0) H5Tclose(dtype);
  if (plist >= 0) H5Pclose(plist);
  if (sid >= 0) H5Sclose(sid);
  return ret;
}

/* 5. Highly Compressible Data Test */
static int test_highly_compressible_data(hid_t fid, const FilterConfig* cfg) {
  #define HC_SIZE 100000
  float *data = NULL, *data_out = NULL;
  hsize_t dims[1] = {HC_SIZE}, chunkdims[1] = {10000};
  hid_t sid = -1, plist = -1, dset = -1;
  int ret = -1;

  data = malloc(HC_SIZE * sizeof(float));
  data_out = malloc(HC_SIZE * sizeof(float));
  if (!data || !data_out) goto cleanup;
  
  for(int i=0; i<HC_SIZE; i++) { data[i] = 0.0f; data_out[i] = -1.0f; }

  CHECK(sid = H5Screate_simple(1, dims, NULL), "H5Screate_simple");
  CHECK(plist = H5Pcreate(H5P_DATASET_CREATE), "H5Pcreate");
  CHECK(H5Pset_chunk(plist, 1, chunkdims), "H5Pset_chunk");
  CHECK(apply_filter(plist, cfg), "apply_filter");

  char dset_name[128];
  snprintf(dset_name, sizeof(dset_name), "/compressible_%s", cfg->name);
  CHECK(dset = H5Dcreate2(fid, dset_name, H5T_NATIVE_FLOAT, sid, H5P_DEFAULT, plist, H5P_DEFAULT), "H5Dcreate2");
  CHECK(H5Dwrite(dset, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, H5P_DEFAULT, data), "H5Dwrite");
  CHECK(dset = reopen_dset(fid, dset, dset_name), "reopen");
  CHECK(H5Dread(dset, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, H5P_DEFAULT, data_out), "H5Dread");

  for(int i=0; i<HC_SIZE; i++) {
    if(data[i] - data_out[i] > 0.1 || data_out[i] - data[i] > 0.1) {
      Rprintf("\n      -> Compressible data mismatch at index %d", i);
      goto cleanup;
    }
  }
  ret = 0;

cleanup:
  if (data) free(data);
  if (data_out) free(data_out);
  CLOSE_DSET(dset);
  if (plist >= 0) H5Pclose(plist);
  if (sid >= 0) H5Sclose(sid);
  return ret;
}

/* 6. Incompressible Data Test */
static int test_incompressible_data(hid_t fid, const FilterConfig* cfg) {
  #define RND_SIZE 10000
  unsigned char *data = NULL, *data_out = NULL;
  hsize_t dims[1] = {RND_SIZE}, chunkdims[1] = {1000};
  hid_t sid = -1, plist = -1, dset = -1;
  int ret = -1;

  data = malloc(RND_SIZE);
  data_out = malloc(RND_SIZE);
  if (!data || !data_out) goto cleanup;
  
  srand(12345);
  for(int i=0; i<RND_SIZE; i++) { 
    data[i] = (unsigned char)(rand() % 256); 
    data_out[i] = 0; 
  }

  CHECK(sid = H5Screate_simple(1, dims, NULL), "H5Screate_simple");
  CHECK(plist = H5Pcreate(H5P_DATASET_CREATE), "H5Pcreate");
  CHECK(H5Pset_chunk(plist, 1, chunkdims), "H5Pset_chunk");
  CHECK(apply_filter(plist, cfg), "apply_filter");

  char dset_name[128];
  snprintf(dset_name, sizeof(dset_name), "/incompressible_%s", cfg->name);
  CHECK(dset = H5Dcreate2(fid, dset_name, H5T_NATIVE_UCHAR, sid, H5P_DEFAULT, plist, H5P_DEFAULT), "H5Dcreate2");
  CHECK(H5Dwrite(dset, H5T_NATIVE_UCHAR, H5S_ALL, H5S_ALL, H5P_DEFAULT, data), "H5Dwrite");
  CHECK(dset = reopen_dset(fid, dset, dset_name), "reopen");
  CHECK(H5Dread(dset, H5T_NATIVE_UCHAR, H5S_ALL, H5S_ALL, H5P_DEFAULT, data_out), "H5Dread");

  for(int i=0; i<RND_SIZE; i++) {
    if(data[i] != data_out[i]) {
      Rprintf("\n      -> Incompressible data mismatch at index %d", i);
      goto cleanup;
    }
  }
  ret = 0;

cleanup:
  if (data) free(data);
  if (data_out) free(data_out);
  CLOSE_DSET(dset);
  if (plist >= 0) H5Pclose(plist);
  if (sid >= 0) H5Sclose(sid);
  return ret;
}

/* 7. Unchunked / Contiguous Data Control Test */
static int test_unchunked_data(hid_t fid, const FilterConfig* cfg) {
  #define UNC_SIZE 1024
  int data[UNC_SIZE], data_out[UNC_SIZE];
  hsize_t dims[1] = {UNC_SIZE};
  hid_t sid = -1, plist = -1, dset = -1;
  int ret = -1;

  for(int i=0; i<UNC_SIZE; i++) { data[i] = i; data_out[i] = -1; }

  CHECK(sid = H5Screate_simple(1, dims, NULL), "H5Screate_simple");
  CHECK(plist = H5Pcreate(H5P_DATASET_CREATE), "H5Pcreate");

  char dset_name[128];
  snprintf(dset_name, sizeof(dset_name), "/unchunked_%s", cfg->name);
  CHECK(dset = H5Dcreate2(fid, dset_name, H5T_NATIVE_INT, sid, H5P_DEFAULT, plist, H5P_DEFAULT), "H5Dcreate2");
  CHECK(H5Dwrite(dset, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, data), "H5Dwrite");
  CHECK(H5Dread(dset, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, data_out), "H5Dread");

  for(int i=0; i<UNC_SIZE; i++) {
    if(data[i] != data_out[i]) {
      Rprintf("\n      -> Unchunked data mismatch at index %d", i);
      goto cleanup;
    }
  }
  ret = 0;

cleanup:
  CLOSE_DSET(dset);
  if (plist >= 0) H5Pclose(plist);
  if (sid >= 0) H5Sclose(sid);
  return ret;
}

/* 8. Empty Dataset Logic Path */
static int test_empty_dataset(hid_t fid, const FilterConfig* cfg) {
  hsize_t dims[1] = {0}; 
  hsize_t maxdims[1] = {H5S_UNLIMITED};
  hsize_t chunkdims[1] = {128};
  hid_t sid = -1, plist = -1, dset = -1;
  int ret = -1;

  CHECK(sid = H5Screate_simple(1, dims, maxdims), "H5Screate_simple");
  CHECK(plist = H5Pcreate(H5P_DATASET_CREATE), "H5Pcreate");
  CHECK(H5Pset_chunk(plist, 1, chunkdims), "H5Pset_chunk");
  CHECK(apply_filter(plist, cfg), "apply_filter");

  char dset_name[128];
  snprintf(dset_name, sizeof(dset_name), "/empty_%s", cfg->name);
  CHECK(dset = H5Dcreate2(fid, dset_name, H5T_NATIVE_FLOAT, sid, H5P_DEFAULT, plist, H5P_DEFAULT), "H5Dcreate2");
  
  float dummy = 0.0f;
  CHECK(H5Dwrite(dset, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, H5P_DEFAULT, &dummy), "H5Dwrite");
  CHECK(H5Dread(dset, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, H5P_DEFAULT, &dummy), "H5Dread");

  ret = 0;

cleanup:
  CLOSE_DSET(dset);
  if (plist >= 0) H5Pclose(plist);
  if (sid >= 0) H5Sclose(sid);
  return ret;
}

/* 9. Two-Dimensional Chunk Logic Path */
static int test_ndim2_data(hid_t fid, const FilterConfig* cfg) {
  #define ND_X 64
  #define ND_Y 64
  unsigned char *data = NULL, *data_out = NULL;
  hsize_t dims[2] = {ND_X, ND_Y}, chunkdims[2] = {32, 32};
  hid_t sid = -1, plist = -1, dset = -1;
  int ret = -1;

  data = malloc(ND_X * ND_Y);
  data_out = malloc(ND_X * ND_Y);
  if (!data || !data_out) goto cleanup;

  /* Structure the data along both axes so the multidimensional codecs have
     something to find that a purely linear scan would miss. */
  for(int x=0; x<ND_X; x++)
    for(int y=0; y<ND_Y; y++) {
      data[x*ND_Y + y] = (unsigned char)((x % 8) * 16 + (y % 8));
      data_out[x*ND_Y + y] = 0;
    }

  CHECK(sid = H5Screate_simple(2, dims, NULL), "H5Screate_simple");
  CHECK(plist = H5Pcreate(H5P_DATASET_CREATE), "H5Pcreate");
  CHECK(H5Pset_chunk(plist, 2, chunkdims), "H5Pset_chunk");
  CHECK(apply_filter(plist, cfg), "apply_filter");

  char dset_name[128];
  snprintf(dset_name, sizeof(dset_name), "/ndim2_%s", cfg->name);
  CHECK(dset = H5Dcreate2(fid, dset_name, H5T_NATIVE_UCHAR, sid, H5P_DEFAULT, plist, H5P_DEFAULT), "H5Dcreate2");
  CHECK(H5Dwrite(dset, H5T_NATIVE_UCHAR, H5S_ALL, H5S_ALL, H5P_DEFAULT, data), "H5Dwrite");

  /* Push every chunk through the pipeline before reading it back, so the
     verification below cannot be answered out of the chunk cache. */
  CHECK(H5Dflush(dset), "H5Dflush (Compression Pipeline Crashed!)");
  CHECK(dset = reopen_dset(fid, dset, dset_name), "reopen");
  CHECK(H5Dread(dset, H5T_NATIVE_UCHAR, H5S_ALL, H5S_ALL, H5P_DEFAULT, data_out), "H5Dread");

  for(int i=0; i < ND_X * ND_Y; i++) {
    if(data[i] != data_out[i]) {
      Rprintf("\n      -> 2-D data mismatch at index %d (in: %d, out: %d)", i, data[i], data_out[i]);
      goto cleanup;
    }
  }
  ret = 0;

cleanup:
  if (data) free(data);
  if (data_out) free(data_out);
  CLOSE_DSET(dset);
  if (plist >= 0) H5Pclose(plist);
  if (sid >= 0) H5Sclose(sid);
  return ret;
}

/* 10. Non-Finite Values Through ZFP */

/* ZFP cannot encode NaN or Inf, so its filters refuse a chunk that holds any.
   An optional filter must then leave just that chunk unfiltered, exact and
   readable; a mandatory one must fail the write rather than store garbage.
   Reversible ZFP reproduces any bit pattern and is expected to keep them all.
   The finite values take both signs, which also drives ZFP's transforms
   through the negative intermediates that once tripped UBSan. */
#define NF_CHUNK  256
#define NF_NCHUNK 4
#define NF_BAD    1   /* index of the chunk that holds the non-finite values */

static herr_t make_filter_optional(hid_t plist, H5Z_filter_t id) {
  unsigned int flags, cd_values[16];
  size_t nelmts = 16;
  if (H5Pget_filter_by_id(plist, id, &flags, &nelmts, cd_values, 0, NULL, NULL) < 0) return -1;
  /* The H5Pset_zfp_*() helpers leave no cd_values; the compression article
     shows that case as H5Pmodify_filter(plist, 32013, H5Z_FLAG_OPTIONAL, 0, NULL) */
  return H5Pmodify_filter(plist, id, H5Z_FLAG_OPTIONAL, nelmts, nelmts ? cd_values : NULL);
}

static int test_nonfinite_data(hid_t fid, const FilterConfig* cfg, unsigned int flags, int use_float) {
  size_t tsize = use_float ? sizeof(float) : sizeof(double);
  hid_t mtype = use_float ? H5T_NATIVE_FLOAT : H5T_NATIVE_DOUBLE;
  hsize_t dims[1] = {NF_CHUNK * NF_NCHUNK}, chunkdims[1] = {NF_CHUNK};
  unsigned char *data = malloc(dims[0] * tsize), *data_out = malloc(dims[0] * tsize);
  hid_t sid = -1, plist = -1, dset = -1;
  int ret = -1;
  int reversible = (cfg->id == H5Z_FILTER_ZFP && cfg->cd_values[0] == 5);

  if (!data || !data_out) goto cleanup;

  for (hsize_t i = 0; i < dims[0]; i++) {
    double v = sin((double)i / 50.0) * 100.0;
    if (use_float) { float f = (float)v; memcpy(data + i * tsize, &f, tsize); }
    else           { memcpy(data + i * tsize, &v, tsize); }
  }
  /* R's NA_real_ is a NaN with a payload; a float can't carry it, so use NaN */
  size_t bad = NF_BAD * NF_CHUNK;
  if (use_float) {
    float nf[4] = {NAN, NAN, INFINITY, -INFINITY};
    for (int k = 0; k < 4; k++) memcpy(data + (bad + 10 * (k + 1)) * tsize, &nf[k], tsize);
  } else {
    uint64_t na = UINT64_C(0x7FF00000000007A2);
    double nf[3] = {NAN, INFINITY, -INFINITY};
    memcpy(data + (bad + 10) * tsize, &na, tsize);
    for (int k = 0; k < 3; k++) memcpy(data + (bad + 10 * (k + 2)) * tsize, &nf[k], tsize);
  }

  CHECK(sid = H5Screate_simple(1, dims, NULL), "H5Screate_simple");
  CHECK(plist = H5Pcreate(H5P_DATASET_CREATE), "H5Pcreate");
  CHECK(H5Pset_chunk(plist, 1, chunkdims), "H5Pset_chunk");
  CHECK(apply_filter(plist, cfg), "apply_filter");
  if (flags == H5Z_FLAG_OPTIONAL) CHECK(make_filter_optional(plist, cfg->id), "make_filter_optional");

  char dset_name[128];
  snprintf(dset_name, sizeof(dset_name), "/nonfinite_%s_%s_%s", cfg->name,
           use_float ? "f32" : "f64", flags == H5Z_FLAG_OPTIONAL ? "opt" : "man");
  CHECK(dset = H5Dcreate2(fid, dset_name, mtype, sid, H5P_DEFAULT, plist, H5P_DEFAULT), "H5Dcreate2");

  herr_t wrote = H5Dwrite(dset, mtype, H5S_ALL, H5S_ALL, H5P_DEFAULT, data);
  if (wrote >= 0) wrote = H5Dflush(dset);

  if (flags == H5Z_FLAG_MANDATORY && !reversible) {
    if (wrote >= 0) {
      Rprintf("\n      -> Mandatory filter accepted non-finite values (%s)", use_float ? "f32" : "f64");
      goto cleanup;
    }
    /* The failed chunk is still dirty, so closing fails again; that is expected */
    H5Dclose(dset);
    dset = -1;
    ret = 0;
    goto cleanup;
  }

  CHECK(wrote, "H5Dwrite/H5Dflush");
  if (H5Dclose(dset) < 0) { dset = -1; Rprintf("\n      -> H5Dclose failed"); goto cleanup; }

  /* Read back through a fresh handle, so nothing comes out of the chunk cache */
  CHECK(dset = H5Dopen2(fid, dset_name, H5P_DEFAULT), "H5Dopen2");
  CHECK(H5Dread(dset, mtype, H5S_ALL, H5S_ALL, H5P_DEFAULT, data_out), "H5Dread");

  for (hsize_t c = 0; c < NF_NCHUNK; c++) {
    hsize_t offset[1], size;
    unsigned mask;
    haddr_t addr;
    CHECK(H5Dget_chunk_info(dset, H5S_ALL, c, offset, &mask, &addr, &size), "H5Dget_chunk_info");
    int skipped = (mask != 0);
    int expect_skipped = (offset[0] / NF_CHUNK == NF_BAD) && !reversible;
    if (skipped != expect_skipped) {
      Rprintf("\n      -> Chunk at %llu was %sfiltered (%s)", (unsigned long long)offset[0],
              skipped ? "not " : "", use_float ? "f32" : "f64");
      goto cleanup;
    }
  }

  /* The non-finite chunk (and everything, if reversible) must be bit-exact */
  if (memcmp(data + bad * tsize, data_out + bad * tsize, NF_CHUNK * tsize) != 0 ||
      (reversible && memcmp(data, data_out, dims[0] * tsize) != 0)) {
    Rprintf("\n      -> Non-finite chunk did not round-trip exactly (%s)", use_float ? "f32" : "f64");
    goto cleanup;
  }
  /* The other chunks went through lossy ZFP */
  for (hsize_t i = 0; i < dims[0]; i++) {
    if (i / NF_CHUNK == NF_BAD) continue;
    double a, b;
    if (use_float) { float fa, fb; memcpy(&fa, data + i * tsize, tsize); memcpy(&fb, data_out + i * tsize, tsize); a = fa; b = fb; }
    else           { memcpy(&a, data + i * tsize, tsize); memcpy(&b, data_out + i * tsize, tsize); }
    if (!(fabs(a - b) <= 1.0 + 0.1 * fabs(a))) {
      Rprintf("\n      -> Mismatch at index %llu (in: %f, out: %f)", (unsigned long long)i, a, b);
      goto cleanup;
    }
  }
  ret = 0;

cleanup:
  if (data) free(data);
  if (data_out) free(data_out);
  CLOSE_DSET(dset);
  if (plist >= 0) H5Pclose(plist);
  if (sid >= 0) H5Sclose(sid);
  return ret;
}

/* 11. Multi-Block Chunks With and Without Threads */

/* Chunks large enough for Blosc to split into many blocks, so that a second
   thread has work to do.  Every chunk holds the same values: with one thread
   the stored bytes must then match chunk for chunk, which shows that a
   compression context reused across chunks writes exactly what a fresh one
   does.  Lossy codecs are held to a tolerance; the rest must be exact. */
#define THR_CHUNK_1D 262144   /* 2 MB of doubles */
#define THR_CHUNK_2D 512      /* 512 x 512 doubles, also 2 MB */

static double thr_value(hsize_t row, hsize_t col) {
  hsize_t k = row * THR_CHUNK_2D + col;
  return floor(sin((double)k / 3000.0) * 1000.0) / 8.0 + (double)(k % 7);
}

static int test_threaded_data(hid_t fid, const FilterConfig* cfg, int nthreads, int rank) {
  hsize_t dims[2], chunkdims[2];
  if (rank == 1) { dims[0] = 3 * THR_CHUNK_1D; chunkdims[0] = THR_CHUNK_1D; }
  else           { dims[0] = dims[1] = 2 * THR_CHUNK_2D; chunkdims[0] = chunkdims[1] = THR_CHUNK_2D; }
  hsize_t n = (rank == 1) ? dims[0] : dims[0] * dims[1];
  hsize_t nchunks = (rank == 1) ? 3 : 4;
  int lossy = (cfg->id == H5Z_FILTER_BLOSC2 && cfg->cd_values[6] >= 33 && cfg->cd_values[6] <= 35);

  double *data = malloc(n * sizeof(double)), *data_out = malloc(n * sizeof(double));
  unsigned char *raw0 = NULL, *raw = NULL;
  hid_t sid = -1, plist = -1, dset = -1;
  int ret = -1;
  int old_nthreads = hdf5lib_set_blosc_nthreads(nthreads);

  if (!data || !data_out) goto cleanup;

  for (hsize_t i = 0; i < n; i++) {
    /* Position within the chunk, so that every chunk holds the same values */
    if (rank == 1) data[i] = thr_value(0, i % THR_CHUNK_1D);
    else           data[i] = thr_value((i / dims[1]) % THR_CHUNK_2D, (i % dims[1]) % THR_CHUNK_2D);
    data_out[i] = -1.0;
  }

  CHECK(sid = H5Screate_simple(rank, dims, NULL), "H5Screate_simple");
  CHECK(plist = H5Pcreate(H5P_DATASET_CREATE), "H5Pcreate");
  CHECK(H5Pset_chunk(plist, rank, chunkdims), "H5Pset_chunk");
  CHECK(apply_filter(plist, cfg), "apply_filter");

  char dset_name[128];
  snprintf(dset_name, sizeof(dset_name), "/threads_%s_%dd_%dt", cfg->name, rank, nthreads);
  CHECK(dset = H5Dcreate2(fid, dset_name, H5T_NATIVE_DOUBLE, sid, H5P_DEFAULT, plist, H5P_DEFAULT), "H5Dcreate2");
  CHECK(H5Dwrite(dset, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, data), "H5Dwrite");
  if (H5Dclose(dset) < 0) { dset = -1; Rprintf("\n      -> H5Dclose failed"); goto cleanup; }

  CHECK(dset = H5Dopen2(fid, dset_name, H5P_DEFAULT), "H5Dopen2");
  CHECK(H5Dread(dset, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, data_out), "H5Dread");

  for (hsize_t i = 0; i < n; i++) {
    double diff = fabs(data[i] - data_out[i]);
    if (lossy ? diff > 1.0 : diff != 0.0) {
      Rprintf("\n      -> %dD/%d-thread mismatch at index %llu (in: %f, out: %f)",
              rank, nthreads, (unsigned long long)i, data[i], data_out[i]);
      goto cleanup;
    }
  }

  if (nthreads == 1) {
    hsize_t size0 = 0;
    for (hsize_t c = 0; c < nchunks; c++) {
      hsize_t offset[2], size;
      unsigned mask;
      haddr_t addr;
      CHECK(H5Dget_chunk_info(dset, H5S_ALL, c, offset, &mask, &addr, &size), "H5Dget_chunk_info");
      if (mask != 0) { Rprintf("\n      -> Chunk %llu was stored unfiltered", (unsigned long long)c); goto cleanup; }

      unsigned char **dest = (c == 0) ? &raw0 : &raw;
      if (*dest) free(*dest);
      if (!(*dest = malloc((size_t)size))) goto cleanup;
      size_t buf_size = (size_t)size;
      CHECK(H5Dread_chunk(dset, H5P_DEFAULT, offset, &mask, *dest, &buf_size), "H5Dread_chunk");

      if (c == 0) size0 = size;
      else if (size != size0 || memcmp(raw0, raw, (size_t)size) != 0) {
        Rprintf("\n      -> Chunk %llu compressed differently from chunk 0 (%llu vs %llu bytes)",
                (unsigned long long)c, (unsigned long long)size, (unsigned long long)size0);
        goto cleanup;
      }
    }
  }
  ret = 0;

cleanup:
  hdf5lib_set_blosc_nthreads(old_nthreads);
  if (data) free(data);
  if (data_out) free(data_out);
  if (raw0) free(raw0);
  if (raw) free(raw);
  CLOSE_DSET(dset);
  if (plist >= 0) H5Pclose(plist);
  if (sid >= 0) H5Sclose(sid);
  return ret;
}

/* 12. Blosc Thread Count Setting */
static int test_blosc_nthreads_api(void) {
  /* BLOSC_NTHREADS overrides the setting, so the checks below assume it is unset */
  if (getenv("BLOSC_NTHREADS") != NULL) return 0;

  int old = hdf5lib_set_blosc_nthreads(0);
  int ok = hdf5lib_get_blosc_nthreads() == 1 &&
           hdf5lib_set_blosc_nthreads(3) == 0 &&
           hdf5lib_get_blosc_nthreads() == 3 &&
           hdf5lib_set_blosc_nthreads(1000) == 3 &&
           hdf5lib_get_blosc_nthreads() == 256 &&
           hdf5lib_set_blosc_nthreads(-1) == 256 &&
           hdf5lib_get_blosc_nthreads() == 1;
  hdf5lib_set_blosc_nthreads(old);
  return ok ? 0 : -1;
}

/* 13. Blosc2 Filter Values That h5py Can Read */

/* hdf5plugin's Blosc2 filter reads a chunk rank from cd_values[7] whenever one
   is there and rejects ranks below 2.  So a 1-D chunk, or one of a rank B2ND
   cannot hold, must store exactly the seven values it writes; ranks 2..16 add
   rank and shape, and nothing after.  Our pre-filters and meta value live in
   slot 0, and slot 5 holds the last pre-filter as the reference names it.
   Trunc-prec makes the meta value observable: keeping 8 bits, the low 15
   mantissa bits of every float32 must read back as zero. */

/* Whether the stored list is exactly the `n` values in `want` */
static int b2_list_is(const unsigned int *vals, size_t nelmts, const unsigned int *want, size_t n) {
  if (nelmts != n) return 0;
  for (size_t i = 0; i < n; i++) if (vals[i] != want[i]) return 0;
  return 1;
}

static void b2_print_list(const char *what, const unsigned int *vals, size_t nelmts) {
  Rprintf("\n      -> %s stored %zu cd_values:", what, nelmts);
  for (size_t i = 0; i < nelmts && i < 32; i++) Rprintf(i ? " %u" : " 0x%08X", vals[i]);
}

static int test_blosc2_layout(hid_t fid, int rank) {
  hsize_t dims[17], chunkdims[17], n = 4096;
  float *data = malloc(n * sizeof(float)), *data_out = malloc(n * sizeof(float));
  unsigned int cd[7] = {B2_SETTINGS(8 | 2, 8), 0, 0, 0, 5, 2, 1};  /* trunc-prec(8) + bitshuffle, lz4 */
  unsigned int vals[32], flags;
  size_t nelmts = 32;
  hid_t sid = -1, plist = -1, dset = -1, dcpl = -1;
  int ret = -1;

  if (!data || !data_out) goto cleanup;

  for (int i = 0; i < rank; i++) dims[i] = chunkdims[i] = 1;
  if (rank == 1) { dims[0] = n; chunkdims[0] = n / 4; }
  else { dims[0] = dims[1] = 64; chunkdims[0] = chunkdims[1] = 32; }
  for (hsize_t i = 0; i < n; i++) data[i] = (float)(sin((double)i / 20.0) * 1000.0);

  CHECK(sid = H5Screate_simple(rank, dims, NULL), "H5Screate_simple");
  CHECK(plist = H5Pcreate(H5P_DATASET_CREATE), "H5Pcreate");
  CHECK(H5Pset_chunk(plist, rank, chunkdims), "H5Pset_chunk");
  CHECK(H5Pset_filter(plist, H5Z_FILTER_BLOSC2, H5Z_FLAG_MANDATORY, 7, cd), "H5Pset_filter");

  char dset_name[64];
  snprintf(dset_name, sizeof(dset_name), "/blosc2_layout_%dd", rank);
  CHECK(dset = H5Dcreate2(fid, dset_name, H5T_NATIVE_FLOAT, sid, H5P_DEFAULT, plist, H5P_DEFAULT), "H5Dcreate2");
  CHECK(H5Dwrite(dset, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, H5P_DEFAULT, data), "H5Dwrite");
  if (H5Dclose(dset) < 0) { dset = -1; Rprintf("\n      -> H5Dclose failed"); goto cleanup; }

  CHECK(dset = H5Dopen2(fid, dset_name, H5P_DEFAULT), "H5Dopen2");
  CHECK(dcpl = H5Dget_create_plist(dset), "H5Dget_create_plist");
  CHECK(H5Pget_filter_by_id(dcpl, H5Z_FILTER_BLOSC2, &flags, &nelmts, vals, 0, NULL, NULL), "H5Pget_filter_by_id");

  unsigned int want[10] = {B2_SETTINGS(10, 8), 0, 4, (unsigned int)(n / 4 * sizeof(float)), 5, 2, 1, 2, 32, 32};
  if (!b2_list_is(vals, nelmts, want, (rank == 2) ? 10 : 7)) {
    char what[32];
    snprintf(what, sizeof(what), "Rank %d", rank);
    b2_print_list(what, vals, nelmts);
    goto cleanup;
  }

  CHECK(H5Dread(dset, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, H5P_DEFAULT, data_out), "H5Dread");
  for (hsize_t i = 0; i < n; i++) {
    uint32_t bits;
    memcpy(&bits, &data_out[i], sizeof(bits));
    if ((bits & 0x7FFF) != 0 || fabs(data[i] - data_out[i]) > fabs(data[i]) / 128.0) {
      Rprintf("\n      -> Rank %d value %llu not truncated to 8 bits (in: %f, out: %f)",
              rank, (unsigned long long)i, data[i], data_out[i]);
      goto cleanup;
    }
  }
  ret = 0;

cleanup:
  if (data) free(data);
  if (data_out) free(data_out);
  if (dcpl >= 0) H5Pclose(dcpl);
  CLOSE_DSET(dset);
  if (plist >= 0) H5Pclose(plist);
  if (sid >= 0) H5Sclose(sid);
  return ret;
}

/* 14. ZFP Leaves No Datatype Open */
static int test_zfp_type_leak(hid_t fid) {
  hsize_t dims[1] = {256};
  ssize_t before, after;
  hid_t sid = -1, plist = -1, dset = -1;
  int ret = -1;

  CHECK(sid = H5Screate_simple(1, dims, NULL), "H5Screate_simple");
  CHECK(before = H5Fget_obj_count(H5F_OBJ_ALL, H5F_OBJ_ALL), "H5Fget_obj_count");
  for (int k = 0; k < 3; k++) {
    char dset_name[64];
    snprintf(dset_name, sizeof(dset_name), "/zfp_type_leak_%d", k);
    CHECK(plist = H5Pcreate(H5P_DATASET_CREATE), "H5Pcreate");
    CHECK(H5Pset_chunk(plist, 1, dims), "H5Pset_chunk");
    CHECK(H5Pset_zfp_rate(plist, 16.0), "H5Pset_zfp_rate");
    CHECK(dset = H5Dcreate2(fid, dset_name, H5T_NATIVE_DOUBLE, sid, H5P_DEFAULT, plist, H5P_DEFAULT), "H5Dcreate2");
    CHECK(H5Dclose(dset), "H5Dclose");
    dset = -1;
    H5Pclose(plist);
    plist = -1;
  }
  CHECK(after = H5Fget_obj_count(H5F_OBJ_ALL, H5F_OBJ_ALL), "H5Fget_obj_count");
  if (after != before) {
    Rprintf("\n      -> %lld IDs left open by 3 ZFP datasets", (long long)(after - before));
    goto cleanup;
  }
  ret = 0;

cleanup:
  CLOSE_DSET(dset);
  if (plist >= 0) H5Pclose(plist);
  if (sid >= 0) H5Sclose(sid);
  return ret;
}

/* 15. ZFP on Chunks It Cannot Take */

/* ZFP compresses 4- or 8-byte numbers in native byte order, in chunks with 1
   to 4 dimensions longer than one.  For anything else a mandatory ZFP filter
   must fail H5Dcreate2(), and an optional one must leave every chunk
   unfiltered and exact, recording values that make a ZFP filter refuse them.
   It used to record no values for a single value or a 5-D chunk (and then
   read cd_values that were not there), fail H5Dcreate2() for int16, and
   compress the other byte order as though it were native. */
static const char *zfp_unsupported_names[] = {"single", "5d", "int16", "swapped"};

static int test_zfp_unsupported(hid_t fid, int which, unsigned int flags) {
  const char *what = zfp_unsupported_names[which];
  hsize_t dims[5] = {2, 2, 2, 2, 2}, n = 1;
  int rank = (which == 1) ? 5 : 1;
  if (which == 0) dims[0] = 1;
  if (which >= 2) dims[0] = 64;
  for (int i = 0; i < rank; i++) n *= dims[i];

  hid_t mtype = (which == 2) ? H5T_NATIVE_SHORT : H5T_NATIVE_DOUBLE, ftype = mtype;
  if (which == 3) ftype = (H5Tget_order(H5T_NATIVE_DOUBLE) == H5T_ORDER_LE) ? H5T_IEEE_F64BE : H5T_IEEE_F64LE;
  size_t tsize = H5Tget_size(mtype);

  unsigned char *data = malloc(n * tsize), *data_out = malloc(n * tsize);
  unsigned int vals[16], fflags;
  size_t nelmts = 16;
  hid_t sid = -1, plist = -1, dset = -1, dcpl = -1;
  int ret = -1;

  if (!data || !data_out) goto cleanup;

  /* Small whole numbers: read in the other byte order they are subnormals,
     which lossy ZFP rounds to zero */
  for (hsize_t i = 0; i < n; i++) {
    if (which == 2) { short s = (short)(3 * (int)i - 50); memcpy(data + i * tsize, &s, tsize); }
    else            { double d = (double)i + 1.0;         memcpy(data + i * tsize, &d, tsize); }
  }

  CHECK(sid = H5Screate_simple(rank, dims, NULL), "H5Screate_simple");
  CHECK(plist = H5Pcreate(H5P_DATASET_CREATE), "H5Pcreate");
  CHECK(H5Pset_chunk(plist, rank, dims), "H5Pset_chunk");
  CHECK(H5Pset_zfp_accuracy(plist, 0.01), "H5Pset_zfp_accuracy");
  if (flags == H5Z_FLAG_OPTIONAL) CHECK(make_filter_optional(plist, H5Z_FILTER_ZFP), "make_filter_optional");

  char dset_name[64];
  snprintf(dset_name, sizeof(dset_name), "/zfp_unsupported_%s_%s", what,
           flags == H5Z_FLAG_OPTIONAL ? "opt" : "man");
  dset = H5Dcreate2(fid, dset_name, ftype, sid, H5P_DEFAULT, plist, H5P_DEFAULT);

  if (flags == H5Z_FLAG_MANDATORY) {
    if (dset >= 0) Rprintf("\n      -> Mandatory ZFP accepted a %s chunk", what);
    else ret = 0;
    goto cleanup;
  }
  CHECK(dset, "H5Dcreate2");

  /* Look at the recorded values before any chunk reaches the filter */
  CHECK(dcpl = H5Dget_create_plist(dset), "H5Dget_create_plist");
  CHECK(H5Pget_filter_by_id(dcpl, H5Z_FILTER_ZFP, &fflags, &nelmts, vals, 0, NULL, NULL), "H5Pget_filter_by_id");
  if (nelmts != 2 || vals[1] != 0) {
    Rprintf("\n      -> %s chunk recorded %zu ZFP values, not the 2 that refuse it", what, nelmts);
    goto cleanup;
  }

  CHECK(H5Dwrite(dset, mtype, H5S_ALL, H5S_ALL, H5P_DEFAULT, data), "H5Dwrite");
  if (H5Dclose(dset) < 0) { dset = -1; Rprintf("\n      -> H5Dclose failed"); goto cleanup; }
  CHECK(dset = H5Dopen2(fid, dset_name, H5P_DEFAULT), "H5Dopen2");
  CHECK(H5Dread(dset, mtype, H5S_ALL, H5S_ALL, H5P_DEFAULT, data_out), "H5Dread");

  hsize_t offset[5], size;
  unsigned mask;
  haddr_t addr;
  CHECK(H5Dget_chunk_info(dset, H5S_ALL, 0, offset, &mask, &addr, &size), "H5Dget_chunk_info");
  if (mask != 1) {
    Rprintf("\n      -> %s chunk went through ZFP", what);
    goto cleanup;
  }
  if (memcmp(data, data_out, n * tsize) != 0) {
    Rprintf("\n      -> %s chunk did not read back exactly", what);
    goto cleanup;
  }
  ret = 0;

cleanup:
  if (data) free(data);
  if (data_out) free(data_out);
  if (dcpl >= 0) H5Pclose(dcpl);
  CLOSE_DSET(dset);
  if (plist >= 0) H5Pclose(plist);
  if (sid >= 0) H5Sclose(sid);
  return ret;
}

/* 16. ZFP Creation Property List Copied From Another Dataset */

/* A dataset's creation property list holds the ZFP header written for its own
   chunks.  A dataset created from a copy of it, as h5repack creates them, needs
   a header for its chunks in the same mode; it used to inherit the first one's
   and compress its chunks as though they had the first one's shape.  A fixed
   rate is stored as bits per block, so it must be rescaled for a chunk with a
   different number of dimensions: 1-D blocks hold 4 values, 2-D blocks 16. */
static int test_zfp_copied_dcpl(hid_t fid, int use_rate) {
  hsize_t dims_a[1] = {256}, dims_b[2] = {32, 32}, chunk_b[2] = {16, 32};
  size_t n = 1024;
  double *data = malloc(n * sizeof(double)), *data_out = malloc(n * sizeof(double));
  unsigned int vals_a[16], vals_b[16], flags;
  size_t n_a = 16, n_b = 16;
  hid_t sid = -1, plist = -1, dset = -1, dcpl = -1, dcpl_b = -1;
  int ret = -1;
  char name_a[64], name_b[64];

  if (!data || !data_out) goto cleanup;
  for (size_t i = 0; i < n; i++) { data[i] = sin((double)i / 20.0) * 100.0; data_out[i] = -1.0; }
  snprintf(name_a, sizeof(name_a), "/zfp_copy_src_%s", use_rate ? "rate" : "acc");
  snprintf(name_b, sizeof(name_b), "/zfp_copy_dst_%s", use_rate ? "rate" : "acc");

  CHECK(sid = H5Screate_simple(1, dims_a, NULL), "H5Screate_simple");
  CHECK(plist = H5Pcreate(H5P_DATASET_CREATE), "H5Pcreate");
  CHECK(H5Pset_chunk(plist, 1, dims_a), "H5Pset_chunk");
  CHECK(use_rate ? H5Pset_zfp_rate(plist, 16.0) : H5Pset_zfp_accuracy(plist, 0.01), "H5Pset_zfp_*");
  CHECK(dset = H5Dcreate2(fid, name_a, H5T_NATIVE_DOUBLE, sid, H5P_DEFAULT, plist, H5P_DEFAULT), "H5Dcreate2");
  CHECK(H5Dwrite(dset, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, data), "H5Dwrite");
  if (H5Dclose(dset) < 0) { dset = -1; Rprintf("\n      -> H5Dclose failed"); goto cleanup; }
  H5Sclose(sid);
  sid = -1;

  /* Reopened, its property list comes from the file alone, without the
     setting the H5Pset_zfp_*() helper left in the original */
  CHECK(dset = H5Dopen2(fid, name_a, H5P_DEFAULT), "H5Dopen2");
  CHECK(dcpl = H5Dget_create_plist(dset), "H5Dget_create_plist");
  CHECK(H5Pget_filter_by_id(dcpl, H5Z_FILTER_ZFP, &flags, &n_a, vals_a, 0, NULL, NULL), "H5Pget_filter_by_id");
  if (H5Dclose(dset) < 0) { dset = -1; Rprintf("\n      -> H5Dclose failed"); goto cleanup; }
  dset = -1;

  CHECK(H5Pset_chunk(dcpl, 2, chunk_b), "H5Pset_chunk");
  CHECK(sid = H5Screate_simple(2, dims_b, NULL), "H5Screate_simple");
  CHECK(dset = H5Dcreate2(fid, name_b, H5T_NATIVE_DOUBLE, sid, H5P_DEFAULT, dcpl, H5P_DEFAULT), "H5Dcreate2");
  CHECK(dcpl_b = H5Dget_create_plist(dset), "H5Dget_create_plist");
  CHECK(H5Pget_filter_by_id(dcpl_b, H5Z_FILTER_ZFP, &flags, &n_b, vals_b, 0, NULL, NULL), "H5Pget_filter_by_id");
  if (n_a == n_b && memcmp(vals_a, vals_b, n_a * sizeof(vals_a[0])) == 0) {
    Rprintf("\n      -> The copy kept the first dataset's ZFP header (%s)", use_rate ? "rate" : "accuracy");
    goto cleanup;
  }

  CHECK(H5Dwrite(dset, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, data), "H5Dwrite");
  if (H5Dclose(dset) < 0) { dset = -1; Rprintf("\n      -> H5Dclose failed"); goto cleanup; }
  CHECK(dset = H5Dopen2(fid, name_b, H5P_DEFAULT), "H5Dopen2");
  CHECK(H5Dread(dset, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, data_out), "H5Dread");

  for (size_t i = 0; i < n; i++) {
    if (!(fabs(data[i] - data_out[i]) <= (use_rate ? 0.5 : 0.01))) {
      Rprintf("\n      -> Mismatch at index %zu (in: %f, out: %f)", i, data[i], data_out[i]);
      goto cleanup;
    }
  }
  /* 16 bits a value, exactly, in two chunks of 32 blocks */
  if (use_rate && H5Dget_storage_size(dset) != n * 16 / 8) {
    Rprintf("\n      -> Fixed rate stored %llu bytes, not %zu", (unsigned long long)H5Dget_storage_size(dset), n * 16 / 8);
    goto cleanup;
  }
  ret = 0;

cleanup:
  if (data) free(data);
  if (data_out) free(data_out);
  if (dcpl_b >= 0) H5Pclose(dcpl_b);
  if (dcpl >= 0) H5Pclose(dcpl);
  CLOSE_DSET(dset);
  if (plist >= 0) H5Pclose(plist);
  if (sid >= 0) H5Sclose(sid);
  return ret;
}

/* 17. ZFP Filter Without Its Parameters */

/* HDF5 passes cd_values = NULL to a filter that has none, and earlier versions
   of the ZFP filter left none for a chunk ZFP could not take.  The filter has
   to refuse such a chunk, both ways, without reading cd_values; it used to read
   cd_values[0] first. */
extern const H5Z_class2_t zfp_class;

static int test_zfp_no_params(void) {
  double chunk[4] = {1.0, 2.0, 3.0, 4.0};
  unsigned int declined[2] = {0, 0};
  int ret = 0;

  for (int reverse = 0; reverse <= 1; reverse++) {
    for (size_t nelmts = 0; nelmts <= 2; nelmts++) {
      size_t buf_size = sizeof(chunk);
      void *buf = H5allocate_memory(buf_size, 0);
      if (!buf) return -1;
      memcpy(buf, chunk, buf_size);
      size_t got = zfp_class.filter(reverse ? H5Z_FLAG_REVERSE : 0, nelmts, nelmts ? declined : NULL,
                                    sizeof(chunk), &buf_size, &buf);
      if (got != 0 || buf_size != sizeof(chunk) || memcmp(buf, chunk, sizeof(chunk)) != 0) {
        Rprintf("\n      -> Filter accepted a chunk with %zu cd_values (%s)", nelmts, reverse ? "read" : "write");
        ret = -1;
      }
      H5free_memory(buf);
    }
  }
  return ret;
}

/* 18. Blosc2 Truncation Rounds Native Floats Only */

/* Truncate-precision zeroes the low bits of every 4- or 8-byte item.  It must
   leave integers, compound records and floats in the other byte order alone
   (with prec 10 it used to read every int32 below 8192 back as 0), and keep
   rounding native floats, alone or in arrays.  A precision Blosc2 cannot apply
   must fail H5Dcreate2() rather than every chunk, and one with more bits than
   the mantissa keeps them all.

   Writes the items through trunc-prec(prec) + bitshuffle + lz4 and reads them
   back.  Returns the pre-filter mask stored in the file, -2 if H5Dcreate2()
   refused, or -1 on any other failure. */
#define TRUNC_N 4096

static int blosc2_trunc_roundtrip(hid_t fid, const char *name, hid_t ftype, hid_t mtype,
                                  int rank, int prec, const void *data, void *data_out) {
  hsize_t dims[2] = {TRUNC_N, 1}, chunkdims[2] = {TRUNC_N / 4, 1};
  if (rank == 2) { dims[0] = dims[1] = 64; chunkdims[0] = chunkdims[1] = 32; }
  unsigned int cd[7] = {B2_SETTINGS(8 | 2, (unsigned int)prec), 0, 0, 0, 5, 2, 1};
  unsigned int vals[32], flags;
  size_t nelmts = 32;
  hid_t sid = -1, plist = -1, dset = -1, dcpl = -1;
  int ret = -1;

  CHECK(sid = H5Screate_simple(rank, dims, NULL), "H5Screate_simple");
  CHECK(plist = H5Pcreate(H5P_DATASET_CREATE), "H5Pcreate");
  CHECK(H5Pset_chunk(plist, rank, chunkdims), "H5Pset_chunk");
  CHECK(H5Pset_filter(plist, H5Z_FILTER_BLOSC2, H5Z_FLAG_MANDATORY, 7, cd), "H5Pset_filter");
  if ((dset = H5Dcreate2(fid, name, ftype, sid, H5P_DEFAULT, plist, H5P_DEFAULT)) < 0) {
    ret = -2;
    goto cleanup;
  }
  CHECK(H5Dwrite(dset, mtype, H5S_ALL, H5S_ALL, H5P_DEFAULT, data), "H5Dwrite");
  if (H5Dclose(dset) < 0) { dset = -1; Rprintf("\n      -> %s: H5Dclose failed", name); goto cleanup; }
  CHECK(dset = H5Dopen2(fid, name, H5P_DEFAULT), "H5Dopen2");
  CHECK(H5Dread(dset, mtype, H5S_ALL, H5S_ALL, H5P_DEFAULT, data_out), "H5Dread");
  CHECK(dcpl = H5Dget_create_plist(dset), "H5Dget_create_plist");
  CHECK(H5Pget_filter_by_id(dcpl, H5Z_FILTER_BLOSC2, &flags, &nelmts, vals, 0, NULL, NULL), "H5Pget_filter_by_id");
  /* Bitshuffle runs last either way; the precision is kept only with trunc-prec */
  ret = (int)((vals[0] >> 16) & 0xF);
  if (vals[5] != 2 || ((vals[0] >> 8) & 0xFF) != ((ret & 8) ? ((unsigned int)prec & 0xFF) : 0)) {
    Rprintf("\n      -> %s: slot 0 = 0x%08X, slot 5 = %u", name, vals[0], vals[5]);
    ret = -1;
  }

cleanup:
  if (dcpl >= 0) H5Pclose(dcpl);
  CLOSE_DSET(dset);
  if (plist >= 0) H5Pclose(plist);
  if (sid >= 0) H5Sclose(sid);
  return ret;
}

/* Whether every value kept at most `keep` mantissa bits and stayed within that precision */
static int f32_truncated(const float *in, const float *out, size_t n, int keep) {
  for (size_t i = 0; i < n; i++) {
    uint32_t bits;
    memcpy(&bits, &out[i], sizeof(bits));
    if ((bits & ((UINT32_C(1) << (23 - keep)) - 1)) != 0 || fabs(in[i] - out[i]) > fabs(in[i]) / (1 << keep)) return 0;
  }
  return 1;
}

static int f64_truncated(const double *in, const double *out, size_t n, int keep) {
  for (size_t i = 0; i < n; i++) {
    uint64_t bits;
    memcpy(&bits, &out[i], sizeof(bits));
    if ((bits & ((UINT64_C(1) << (52 - keep)) - 1)) != 0 || fabs(in[i] - out[i]) > fabs(in[i]) / ldexp(1.0, keep)) return 0;
  }
  return 1;
}

static int test_blosc2_trunc_types(hid_t fid) {
  int32_t *ints = malloc(TRUNC_N * 3 * sizeof(int32_t)), *ints_out = malloc(TRUNC_N * 3 * sizeof(int32_t));
  float *flts = malloc(TRUNC_N * 4 * sizeof(float)), *flts_out = malloc(TRUNC_N * 4 * sizeof(float));
  double *dbls = malloc(TRUNC_N * sizeof(double)), *dbls_out = malloc(TRUNC_N * sizeof(double));
  hsize_t arr_dims[1] = {4};
  hid_t cmp8 = -1, cmp12 = -1, arr = -1;
  hid_t f64_swapped = (H5Tget_order(H5T_NATIVE_DOUBLE) == H5T_ORDER_LE) ? H5T_IEEE_F64BE : H5T_IEEE_F64LE;
  int errors = 0, mask;
  char name[64];

  if (!ints || !ints_out || !flts || !flts_out || !dbls || !dbls_out) { errors++; goto cleanup; }
  for (int i = 0; i < TRUNC_N * 3; i++) ints[i] = i % 8000;
  for (int i = 0; i < TRUNC_N * 4; i++) flts[i] = (float)(sin(i / 20.0) * 1000.0);
  for (int i = 0; i < TRUNC_N; i++) dbls[i] = sin(i / 20.0) * 1000.0;

  if ((cmp8 = H5Tcreate(H5T_COMPOUND, 8)) < 0 ||
      H5Tinsert(cmp8, "a", 0, H5T_NATIVE_INT32) < 0 || H5Tinsert(cmp8, "b", 4, H5T_NATIVE_INT32) < 0 ||
      (cmp12 = H5Tcreate(H5T_COMPOUND, 12)) < 0 || H5Tinsert(cmp12, "a", 0, H5T_NATIVE_INT32) < 0 ||
      H5Tinsert(cmp12, "b", 4, H5T_NATIVE_INT32) < 0 || H5Tinsert(cmp12, "c", 8, H5T_NATIVE_INT32) < 0 ||
      (arr = H5Tarray_create2(H5T_NATIVE_FLOAT, 1, arr_dims)) < 0) { errors++; goto cleanup; }

  /* Integers, in 1-D and 2-D chunks (stored with and without B2ND geometry) */
  for (int rank = 1; rank <= 2; rank++) {
    snprintf(name, sizeof(name), "/b2trunc_int32_%dd", rank);
    mask = blosc2_trunc_roundtrip(fid, name, H5T_NATIVE_INT32, H5T_NATIVE_INT32, rank, 10, ints, ints_out);
    if (mask != 2 || memcmp(ints, ints_out, TRUNC_N * sizeof(int32_t)) != 0) {
      Rprintf("\n      -> %s: mask %d, or values changed", name, mask);
      errors++;
    }
  }

  /* Compound records: 8 bytes look like doubles to Blosc2; 12 bytes it cannot truncate at all */
  mask = blosc2_trunc_roundtrip(fid, "/b2trunc_cmp8", cmp8, cmp8, 1, 10, ints, ints_out);
  if (mask != 2 || memcmp(ints, ints_out, TRUNC_N * 8) != 0) {
    Rprintf("\n      -> 8-byte compound: mask %d, or values changed", mask);
    errors++;
  }
  mask = blosc2_trunc_roundtrip(fid, "/b2trunc_cmp12", cmp12, cmp12, 1, 10, ints, ints_out);
  if (mask != 2 || memcmp(ints, ints_out, TRUNC_N * 12) != 0) {
    Rprintf("\n      -> 12-byte compound: mask %d, or values changed", mask);
    errors++;
  }

  /* Doubles in the other byte order: their bytes are not the mantissa it would clear */
  mask = blosc2_trunc_roundtrip(fid, "/b2trunc_f64_swapped", f64_swapped, H5T_NATIVE_DOUBLE, 1, 10, dbls, dbls_out);
  if (mask != 2 || memcmp(dbls, dbls_out, TRUNC_N * sizeof(double)) != 0) {
    Rprintf("\n      -> Byte-swapped float64: mask %d, or values changed", mask);
    errors++;
  }

  /* Native floats are still rounded, in arrays too */
  mask = blosc2_trunc_roundtrip(fid, "/b2trunc_f32_array", arr, arr, 1, 10, flts, flts_out);
  if (mask != 10 || !f32_truncated(flts, flts_out, TRUNC_N * 4, 10)) {
    Rprintf("\n      -> Array of float32: mask %d, or not truncated to 10 bits", mask);
    errors++;
  }
  mask = blosc2_trunc_roundtrip(fid, "/b2trunc_f64_30", H5T_NATIVE_DOUBLE, H5T_NATIVE_DOUBLE, 1, 30, dbls, dbls_out);
  if (mask != 10 || !f64_truncated(dbls, dbls_out, TRUNC_N, 30)) {
    Rprintf("\n      -> float64: mask %d, or not truncated to 30 bits", mask);
    errors++;
  }

  /* 30 bits is more than a float32 has, so it keeps them all */
  mask = blosc2_trunc_roundtrip(fid, "/b2trunc_f32_30", H5T_NATIVE_FLOAT, H5T_NATIVE_FLOAT, 1, 30, flts, flts_out);
  if (mask != 2 || memcmp(flts, flts_out, TRUNC_N * sizeof(float)) != 0) {
    Rprintf("\n      -> float32 keeping 30 bits: mask %d, or values changed", mask);
    errors++;
  }

  /* Keeping no bits, or dropping all 23, is refused up front */
  int bad_prec[2] = {0, -23};
  for (int k = 0; k < 2; k++) {
    snprintf(name, sizeof(name), "/b2trunc_f32_bad%d", k);
    mask = blosc2_trunc_roundtrip(fid, name, H5T_NATIVE_FLOAT, H5T_NATIVE_FLOAT, 1, bad_prec[k], flts, flts_out);
    if (mask != -2) {
      Rprintf("\n      -> float32 with precision %d: H5Dcreate2() accepted it (mask %d)", bad_prec[k], mask);
      errors++;
    }
  }

cleanup:
  if (ints) free(ints);
  if (ints_out) free(ints_out);
  if (flts) free(flts);
  if (flts_out) free(flts_out);
  if (dbls) free(dbls);
  if (dbls_out) free(dbls_out);
  if (cmp8 >= 0) H5Tclose(cmp8);
  if (cmp12 >= 0) H5Tclose(cmp12);
  if (arr >= 0) H5Tclose(arr);
  return errors ? -1 : 0;
}

/* 19. Blosc2 Creation Property List Copied From Another Dataset */

/* A dataset created from a copy of another's creation property list, as
   h5repack creates them, must keep the meta value.  For chunks with B2ND
   geometry the list holds the rank and shape after slot 6, and the rank used
   to be taken for the meta value: a 2-D copy kept 2 bits instead of 8, or
   turned ZFP's accuracy of 10^-3 into 10^2.  It now lives in slot 0. */
static int test_blosc2_copied_dcpl(hid_t fid, int rank, int zfp) {
  hsize_t dims[2] = {4096, 1}, chunkdims[2] = {1024, 1};
  if (rank == 2) { dims[0] = dims[1] = 64; chunkdims[0] = chunkdims[1] = 32; }
  size_t n = 4096;
  unsigned int meta = zfp ? 253 : 8;  /* accuracy 10^-3, or keep 8 bits */
  unsigned int cd[7] = {B2_SETTINGS(zfp ? 0 : 8 | 2, meta), 0, 0, 0, 5, zfp ? 0 : 2, zfp ? 33 : 1};
  float *data = malloc(n * sizeof(float)), *data_out = malloc(n * sizeof(float));
  unsigned int vals[32], flags;
  size_t nelmts = 32;
  hid_t sid = -1, plist = -1, dset = -1, dcpl = -1, dcpl_b = -1;
  int ret = -1;
  char name_a[64], name_b[64];

  if (!data || !data_out) goto cleanup;
  for (size_t i = 0; i < n; i++) { data[i] = (float)(sin((double)i / 20.0) * (zfp ? 10.0 : 1000.0)); data_out[i] = -1.0f; }
  snprintf(name_a, sizeof(name_a), "/b2copy_src_%s_%dd", zfp ? "zfp" : "trunc", rank);
  snprintf(name_b, sizeof(name_b), "/b2copy_dst_%s_%dd", zfp ? "zfp" : "trunc", rank);

  CHECK(sid = H5Screate_simple(rank, dims, NULL), "H5Screate_simple");
  CHECK(plist = H5Pcreate(H5P_DATASET_CREATE), "H5Pcreate");
  CHECK(H5Pset_chunk(plist, rank, chunkdims), "H5Pset_chunk");
  CHECK(H5Pset_filter(plist, H5Z_FILTER_BLOSC2, H5Z_FLAG_MANDATORY, 7, cd), "H5Pset_filter");
  CHECK(dset = H5Dcreate2(fid, name_a, H5T_NATIVE_FLOAT, sid, H5P_DEFAULT, plist, H5P_DEFAULT), "H5Dcreate2");
  if (H5Dclose(dset) < 0) { dset = -1; Rprintf("\n      -> H5Dclose failed"); goto cleanup; }

  CHECK(dset = H5Dopen2(fid, name_a, H5P_DEFAULT), "H5Dopen2");
  CHECK(dcpl = H5Dget_create_plist(dset), "H5Dget_create_plist");
  if (H5Dclose(dset) < 0) { dset = -1; Rprintf("\n      -> H5Dclose failed"); goto cleanup; }
  dset = -1;

  CHECK(dset = H5Dcreate2(fid, name_b, H5T_NATIVE_FLOAT, sid, H5P_DEFAULT, dcpl, H5P_DEFAULT), "H5Dcreate2");
  CHECK(H5Dwrite(dset, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, H5P_DEFAULT, data), "H5Dwrite");
  if (H5Dclose(dset) < 0) { dset = -1; Rprintf("\n      -> H5Dclose failed"); goto cleanup; }
  CHECK(dset = H5Dopen2(fid, name_b, H5P_DEFAULT), "H5Dopen2");
  CHECK(H5Dread(dset, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, H5P_DEFAULT, data_out), "H5Dread");
  CHECK(dcpl_b = H5Dget_create_plist(dset), "H5Dget_create_plist");
  CHECK(H5Pget_filter_by_id(dcpl_b, H5Z_FILTER_BLOSC2, &flags, &nelmts, vals, 0, NULL, NULL), "H5Pget_filter_by_id");

  /* 32 x 32 chunks keep their B2ND geometry, ZFP's too */
  unsigned int copied = (vals[0] >> 8) & 0xFF;
  if (copied != meta || vals[0] != cd[0] || nelmts != (rank == 2 ? 10u : 7u)) {
    Rprintf("\n      -> %dD %s copy recorded meta value %u, not %u (slot 0 = 0x%08X, %zu values)",
            rank, zfp ? "ZFP" : "trunc-prec", copied, meta, vals[0], nelmts);
    goto cleanup;
  }
  if (zfp) {
    for (size_t i = 0; i < n; i++) {
      if (!(fabs(data[i] - data_out[i]) <= 0.01)) {
        Rprintf("\n      -> %dD ZFP copy mismatch at index %zu (in: %f, out: %f)", rank, i, data[i], data_out[i]);
        goto cleanup;
      }
    }
  } else if (!f32_truncated(data, data_out, n, 8)) {
    Rprintf("\n      -> %dD trunc-prec copy not truncated to 8 bits", rank);
    goto cleanup;
  }
  ret = 0;

cleanup:
  if (data) free(data);
  if (data_out) free(data_out);
  if (dcpl_b >= 0) H5Pclose(dcpl_b);
  if (dcpl >= 0) H5Pclose(dcpl);
  CLOSE_DSET(dset);
  if (plist >= 0) H5Pclose(plist);
  if (sid >= 0) H5Sclose(sid);
  return ret;
}

/* 20. Blosc2's ZFP Codecs Take Only Native Floats */

/* The ZFP codecs compress whatever they are handed as native floats, so
   integers and byte-swapped floats used to read back as garbage (small
   integers as 0) with no error anywhere.  A mandatory filter must now fail
   H5Dcreate2() on such data, and an optional one store it unfiltered and
   exact, recording compression level 255 so that any Blosc2 filter refuses
   its chunks.  A dataset created from a copy of that list, with data ZFP can
   take, gets the default level back; any other level above 9 is refused up
   front instead of failing every chunk. */
static hid_t blosc2_plist(unsigned int flags, unsigned int clevel, unsigned int compcode, unsigned int meta) {
  hsize_t chunkdims[1] = {TRUNC_N / 4};
  unsigned int cd[7] = {B2_SETTINGS(0, meta), 0, 0, 0, clevel, 0, compcode};
  hid_t plist = H5Pcreate(H5P_DATASET_CREATE);
  if (plist >= 0 && (H5Pset_chunk(plist, 1, chunkdims) < 0 ||
                     H5Pset_filter(plist, H5Z_FILTER_BLOSC2, flags, 7, cd) < 0)) {
    H5Pclose(plist);
    plist = -1;
  }
  return plist;
}

/* Creates a 1-D dataset of TRUNC_N items from plist, writes them, and reads
   them back through a fresh handle, along with the stored compression level
   and the first chunk's filter mask.  Returns -2 if H5Dcreate2() refused. */
static int blosc2_write_read(hid_t fid, const char *name, hid_t ftype, hid_t mtype, hid_t plist,
                             const void *data, void *data_out, unsigned int *clevel, unsigned int *mask) {
  hsize_t dims[1] = {TRUNC_N}, offset[1], size;
  unsigned int vals[32], flags;
  size_t nelmts = 32;
  haddr_t addr;
  hid_t sid = -1, dset = -1, dcpl = -1;
  int ret = -1;

  CHECK(sid = H5Screate_simple(1, dims, NULL), "H5Screate_simple");
  if ((dset = H5Dcreate2(fid, name, ftype, sid, H5P_DEFAULT, plist, H5P_DEFAULT)) < 0) {
    ret = -2;
    goto cleanup;
  }
  CHECK(H5Dwrite(dset, mtype, H5S_ALL, H5S_ALL, H5P_DEFAULT, data), "H5Dwrite");
  if (H5Dclose(dset) < 0) { dset = -1; Rprintf("\n      -> %s: H5Dclose failed", name); goto cleanup; }
  CHECK(dset = H5Dopen2(fid, name, H5P_DEFAULT), "H5Dopen2");
  CHECK(H5Dread(dset, mtype, H5S_ALL, H5S_ALL, H5P_DEFAULT, data_out), "H5Dread");
  CHECK(dcpl = H5Dget_create_plist(dset), "H5Dget_create_plist");
  CHECK(H5Pget_filter_by_id(dcpl, H5Z_FILTER_BLOSC2, &flags, &nelmts, vals, 0, NULL, NULL), "H5Pget_filter_by_id");
  CHECK(H5Dget_chunk_info(dset, H5S_ALL, 0, offset, mask, &addr, &size), "H5Dget_chunk_info");
  *clevel = vals[4];
  ret = 0;

cleanup:
  if (dcpl >= 0) H5Pclose(dcpl);
  CLOSE_DSET(dset);
  if (sid >= 0) H5Sclose(sid);
  return ret;
}

static int test_blosc2_zfp_types(hid_t fid) {
  int32_t *ints = malloc(TRUNC_N * sizeof(int32_t)), *ints_out = malloc(TRUNC_N * sizeof(int32_t));
  double *dbls = malloc(TRUNC_N * sizeof(double)), *dbls_out = malloc(TRUNC_N * sizeof(double));
  hid_t f64_swapped = (H5Tget_order(H5T_NATIVE_DOUBLE) == H5T_ORDER_LE) ? H5T_IEEE_F64BE : H5T_IEEE_F64LE;
  hid_t plist = -1, dset = -1, dcpl = -1;
  unsigned int clevel = 0, mask = 0;
  int errors = 0, rc;

  if (!ints || !ints_out || !dbls || !dbls_out) { errors++; goto cleanup; }
  for (int i = 0; i < TRUNC_N; i++) { ints[i] = i + 1; dbls[i] = sin(i / 20.0) * 100.0; }

  /* Optional: stored unfiltered and exact, at level 255 (ZFP accuracy 10^-3) */
  for (int k = 0; k < 2; k++) {
    const char *name = k ? "/b2zfp_f64_swapped" : "/b2zfp_int32";
    if ((plist = blosc2_plist(H5Z_FLAG_OPTIONAL, 5, 33, 253)) < 0) { errors++; goto cleanup; }
    rc = blosc2_write_read(fid, name, k ? f64_swapped : H5T_NATIVE_INT32, k ? H5T_NATIVE_DOUBLE : H5T_NATIVE_INT32,
                           plist, k ? (void *)dbls : (void *)ints, k ? (void *)dbls_out : (void *)ints_out, &clevel, &mask);
    H5Pclose(plist);
    plist = -1;
    if (rc != 0 || clevel != 255 || mask != 1 ||
        memcmp(k ? (void *)dbls : (void *)ints, k ? (void *)dbls_out : (void *)ints_out,
               TRUNC_N * (k ? sizeof(double) : sizeof(int32_t))) != 0) {
      Rprintf("\n      -> %s: level %u, chunk mask %u, or values changed", name, clevel, mask);
      errors++;
    }
  }

  /* Mandatory: refused at creation */
  if ((plist = blosc2_plist(H5Z_FLAG_MANDATORY, 5, 33, 253)) < 0) { errors++; goto cleanup; }
  if (blosc2_write_read(fid, "/b2zfp_int32_man", H5T_NATIVE_INT32, H5T_NATIVE_INT32, plist,
                        ints, ints_out, &clevel, &mask) != -2) {
    Rprintf("\n      -> Mandatory ZFP codec accepted int32 data");
    errors++;
  }
  H5Pclose(plist);
  plist = -1;

  /* A copy of the declined list, on float64: the default level, compressed.
     The copied layout records int32's size, so set the chunk shape afresh. */
  hsize_t chunkdims[1] = {TRUNC_N / 4};
  if ((dset = H5Dopen2(fid, "/b2zfp_int32", H5P_DEFAULT)) >= 0) {
    dcpl = H5Dget_create_plist(dset);
    H5Dclose(dset);
    dset = -1;
  }
  if (dcpl >= 0 && H5Pset_chunk(dcpl, 1, chunkdims) < 0) { H5Pclose(dcpl); dcpl = -1; }
  rc = (dcpl < 0) ? -1 : blosc2_write_read(fid, "/b2zfp_copy_f64", H5T_NATIVE_DOUBLE, H5T_NATIVE_DOUBLE, dcpl,
                                           dbls, dbls_out, &clevel, &mask);
  int close_enough = 1;
  for (int i = 0; rc == 0 && i < TRUNC_N; i++)
    if (!(fabs(dbls[i] - dbls_out[i]) <= 0.01)) close_enough = 0;
  if (rc != 0 || clevel != 5 || mask != 0 || !close_enough) {
    Rprintf("\n      -> Copy onto float64: level %u, chunk mask %u, or values off", clevel, mask);
    errors++;
  }

  /* A level above 9 is refused at creation, not chunk by chunk */
  if ((plist = blosc2_plist(H5Z_FLAG_MANDATORY, 12, 1, 0)) < 0) { errors++; goto cleanup; }
  if (blosc2_write_read(fid, "/b2_clevel12", H5T_NATIVE_DOUBLE, H5T_NATIVE_DOUBLE, plist,
                        dbls, dbls_out, &clevel, &mask) != -2) {
    Rprintf("\n      -> Compression level 12 was accepted");
    errors++;
  }

cleanup:
  if (ints) free(ints);
  if (ints_out) free(ints_out);
  if (dbls) free(dbls);
  if (dbls_out) free(dbls_out);
  if (dcpl >= 0) H5Pclose(dcpl);
  if (plist >= 0) H5Pclose(plist);
  return errors ? -1 : 0;
}

/* 21. Read Shims Claim Only Quantizers' IDs */

/* The BitGroom, Granular BitRound and BitRound shims hand data back
   untouched, which is right for those quantizers alone.  The BitRound one
   used to sit on 32024, SZ3's ID: SZ3 data then failed to read, and HDF5
   never loaded a real SZ3 plugin. */
static int test_shim_ids(void) {
  H5Z_filter_t shims[] = {H5Z_FILTER_BITGROOM, H5Z_FILTER_GRANULARBR, H5Z_FILTER_BITROUND};
  unsigned int state;
  int ret = 0;

  /* Look in HDF5's filter table only, not in the plugin directories */
  if (H5PLget_loading_state(&state) < 0 || H5PLset_loading_state(0) < 0) return -1;
  for (size_t i = 0; i < sizeof(shims) / sizeof(shims[0]); i++) {
    if (H5Zfilter_avail(shims[i]) <= 0) {
      Rprintf("\n      -> No shim registered for filter %d", (int)shims[i]);
      ret = -1;
    }
  }
  if (H5Zfilter_avail(H5Z_FILTER_SZ3) != 0) {
    Rprintf("\n      -> A filter is registered under SZ3's ID, %d", H5Z_FILTER_SZ3);
    ret = -1;
  }
  H5PLset_loading_state(state);
  return ret;
}

/* 22. Truncated Chunks Fail Cleanly */

/* Every filter must refuse a chunk cut short -- not hang (bzip2 used to spin
   forever), read past the end of it (Blosc, Bitshuffle and ZFP trusted sizes
   in the data), or return it with an unset tail (Snappy).  Writes one chunk,
   copies it into a second dataset with half its bytes, and reads that back. */
static int truncated_read_fails(hid_t fid, const char *name, hid_t plist) {
  hsize_t dims[1] = {4096}, offset[1] = {0}, size = 0;
  double *data = malloc(4096 * sizeof(double)), *data_out = malloc(4096 * sizeof(double));
  unsigned char *raw = NULL;
  uint32_t mask = 0;
  char cut_name[64];
  hid_t sid = -1, dset = -1, cut = -1;
  int ret = -1;

  if (!data || !data_out) goto cleanup;
  for (int i = 0; i < 4096; i++) data[i] = sin(i / 20.0) * 100.0 + (i % 7);
  snprintf(cut_name, sizeof(cut_name), "%s_cut", name);

  CHECK(sid = H5Screate_simple(1, dims, NULL), "H5Screate_simple");
  CHECK(dset = H5Dcreate2(fid, name, H5T_NATIVE_DOUBLE, sid, H5P_DEFAULT, plist, H5P_DEFAULT), "H5Dcreate2");
  CHECK(H5Dwrite(dset, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, data), "H5Dwrite");
  CHECK(dset = reopen_dset(fid, dset, name), "reopen");
  CHECK(H5Dget_chunk_storage_size(dset, offset, &size), "H5Dget_chunk_storage_size");
  if (size < 4 || !(raw = malloc((size_t)size))) goto cleanup;
  size_t raw_size = (size_t)size;
  CHECK(H5Dread_chunk(dset, H5P_DEFAULT, offset, &mask, raw, &raw_size), "H5Dread_chunk");

  CHECK(cut = H5Dcreate2(fid, cut_name, H5T_NATIVE_DOUBLE, sid, H5P_DEFAULT, plist, H5P_DEFAULT), "H5Dcreate2");
  CHECK(H5Dwrite_chunk(cut, H5P_DEFAULT, mask, offset, (size_t)size / 2, raw), "H5Dwrite_chunk");
  if (H5Dread(cut, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, data_out) >= 0) {
    Rprintf("\n      -> %s: a chunk of %llu bytes cut to %llu read without error", name,
            (unsigned long long)size, (unsigned long long)size / 2);
    goto cleanup;
  }
  ret = 0;

cleanup:
  if (data) free(data);
  if (data_out) free(data_out);
  if (raw) free(raw);
  if (cut >= 0) H5Dclose(cut);
  CLOSE_DSET(dset);
  if (sid >= 0) H5Sclose(sid);
  return ret;
}

static int test_truncated_chunks(hid_t fid) {
  struct { const char *name; H5Z_filter_t id; size_t n; unsigned int cd[8]; } cfgs[] = {
    {"/cut_blosc",      H5Z_FILTER_BLOSC,  7, {0, 0, 0, 0, 5, 1, 1}},
    {"/cut_blosc2",     H5Z_FILTER_BLOSC2, 7, {B2_SETTINGS(1, 0), 0, 0, 0, 5, 1, 1}},
    {"/cut_bzip2",      H5Z_FILTER_BZIP2,  1, {9}},
    {"/cut_lz4",        H5Z_FILTER_LZ4,    2, {0, 0}},
    {"/cut_lzf",        H5Z_FILTER_LZF,    0, {0}},
    {"/cut_snappy",     H5Z_FILTER_SNAPPY, 0, {0}},
    {"/cut_zstd",       H5Z_FILTER_ZSTD,   1, {3}},
    {"/cut_bshuf_lz4",  H5Z_FILTER_BSHUF,  2, {0, 2}},
    {"/cut_bshuf_zstd", H5Z_FILTER_BSHUF,  3, {0, 3, 3}},
    {"/cut_zfp",        H5Z_FILTER_ZFP,    0, {0}},
  };
  hsize_t chunkdims[1] = {4096};
  int errors = 0;

  for (size_t k = 0; k < sizeof(cfgs) / sizeof(cfgs[0]); k++) {
    hid_t plist = H5Pcreate(H5P_DATASET_CREATE);
    herr_t set = (plist < 0) ? -1 : H5Pset_chunk(plist, 1, chunkdims);
    if (set >= 0) set = (cfgs[k].id == H5Z_FILTER_ZFP)
      ? H5Pset_zfp_accuracy(plist, 0.01)
      : H5Pset_filter(plist, cfgs[k].id, H5Z_FLAG_MANDATORY, cfgs[k].n, cfgs[k].cd);
    if (set < 0 || truncated_read_fails(fid, cfgs[k].name, plist) < 0) errors++;
    if (plist >= 0) H5Pclose(plist);
  }
  return errors ? -1 : 0;
}

/* 23. Blosc2 Chunks Whose Bytes B2ND Geometry Does Not Describe */

/* B2ND compressed only as much of a chunk as the recorded geometry covers: an
   array datatype's chunks (whose items are the array's base elements), and
   chunks another filter resized first, read back as errors.  The ZFP codecs
   also failed every chunk of a rank above 4 or with a dimension under 4, and
   a list of fewer than 7 values (which the reference filter writes for 1-D
   chunks) could not be read at all. */
extern const H5Z_class2_t blosc2_class;

static int blosc2_case(hid_t fid, const char *name, hid_t type, int rank, const hsize_t *dims,
                       const hsize_t *chunk, int fletcher_first, unsigned int compcode,
                       unsigned int meta, const void *data, void *data_out, size_t n, double tol) {
  /* Bitshuffle, which NDLZ skips (and the stored list then drops) */
  unsigned int cd[7] = {B2_SETTINGS(compcode >= 33 ? 0 : 2, meta), 0, 0, 0, 5, compcode >= 33 ? 0 : 2, compcode};
  hid_t sid = -1, plist = -1, dset = -1;
  int ret = -1;

  CHECK(sid = H5Screate_simple(rank, dims, NULL), "H5Screate_simple");
  CHECK(plist = H5Pcreate(H5P_DATASET_CREATE), "H5Pcreate");
  CHECK(H5Pset_chunk(plist, rank, chunk), "H5Pset_chunk");
  if (fletcher_first) CHECK(H5Pset_fletcher32(plist), "H5Pset_fletcher32");
  CHECK(H5Pset_filter(plist, H5Z_FILTER_BLOSC2, H5Z_FLAG_MANDATORY, 7, cd), "H5Pset_filter");
  CHECK(dset = H5Dcreate2(fid, name, type, sid, H5P_DEFAULT, plist, H5P_DEFAULT), "H5Dcreate2");
  CHECK(H5Dwrite(dset, type, H5S_ALL, H5S_ALL, H5P_DEFAULT, data), "H5Dwrite");
  CHECK(dset = reopen_dset(fid, dset, name), "reopen");
  CHECK(H5Dread(dset, type, H5S_ALL, H5S_ALL, H5P_DEFAULT, data_out), "H5Dread");
  for (size_t i = 0; i < n; i++) {
    double a, b;
    if (H5Tget_class(type) == H5T_ARRAY || H5Tget_size(type) == 4) { a = ((const float *)data)[i]; b = ((float *)data_out)[i]; }
    else                                                          { a = ((const double *)data)[i]; b = ((double *)data_out)[i]; }
    if (!(fabs(a - b) <= tol)) {
      Rprintf("\n      -> %s: mismatch at %zu (in: %f, out: %f)", name, i, a, b);
      goto cleanup;
    }
  }
  ret = 0;

cleanup:
  CLOSE_DSET(dset);
  if (plist >= 0) H5Pclose(plist);
  if (sid >= 0) H5Sclose(sid);
  return ret;
}

static int test_blosc2_geometry(hid_t fid) {
  float *f = malloc(4096 * 4 * sizeof(float)), *f_out = malloc(4096 * 4 * sizeof(float));
  double *d = malloc(4096 * sizeof(double)), *d_out = malloc(4096 * sizeof(double));
  hsize_t adims[1] = {4};
  hsize_t d2[2] = {64, 64}, c2[2] = {32, 32};
  hsize_t d5[5] = {4, 4, 4, 8, 8}, d_narrow[2] = {2048, 2}, d1[1] = {4096}, c1[1] = {1024};
  hid_t arr = -1;
  int errors = 0;

  if (!f || !f_out || !d || !d_out) { errors++; goto cleanup; }
  for (int i = 0; i < 4096 * 4; i++) f[i] = (float)(i % 1000) * 0.5f;
  for (int i = 0; i < 4096; i++) d[i] = sin(i / 20.0) * 10.0;
  if ((arr = H5Tarray_create2(H5T_NATIVE_FLOAT, 1, adims)) < 0) { errors++; goto cleanup; }

  /* Arrays, lossless: 2-D chunks, and NDLZ on 1-D chunks */
  if (blosc2_case(fid, "/b2geo_array_2d", arr, 2, d2, c2, 0, 1, 0, f, f_out, 4096 * 4, 0.0) < 0) errors++;
  if (blosc2_case(fid, "/b2geo_array_ndlz", arr, 1, d1, c1, 0, 32, 4, f, f_out, 4096 * 4, 0.0) < 0) errors++;
  /* Fletcher32 ahead of Blosc2 adds 4 bytes to each chunk */
  if (blosc2_case(fid, "/b2geo_fletcher", H5T_NATIVE_DOUBLE, 2, d2, c2, 1, 1, 0, d, d_out, 4096, 0.0) < 0) errors++;
  /* ZFP accuracy 10^-3 on chunk shapes its B2ND codec cannot take */
  if (blosc2_case(fid, "/b2geo_zfp_5d", H5T_NATIVE_DOUBLE, 5, d5, d5, 0, 33, 253, d, d_out, 4096, 0.01) < 0) errors++;
  if (blosc2_case(fid, "/b2geo_zfp_narrow", H5T_NATIVE_DOUBLE, 2, d_narrow, d_narrow, 0, 33, 253, d, d_out, 4096, 0.01) < 0) errors++;

  /* A chunk too small for a ZFP block: refused at creation when mandatory */
  {
    hsize_t two[1] = {2};
    unsigned int cd[7] = {B2_SETTINGS(0, 253), 0, 0, 0, 5, 0, 33};
    hid_t sid = H5Screate_simple(1, two, NULL), plist = H5Pcreate(H5P_DATASET_CREATE);
    H5Pset_chunk(plist, 1, two);
    H5Pset_filter(plist, H5Z_FILTER_BLOSC2, H5Z_FLAG_MANDATORY, 7, cd);
    hid_t dset = H5Dcreate2(fid, "/b2geo_zfp_two", H5T_NATIVE_DOUBLE, sid, H5P_DEFAULT, plist, H5P_DEFAULT);
    if (dset >= 0) { Rprintf("\n      -> Mandatory ZFP codec accepted a 2-value chunk"); errors++; H5Dclose(dset); }
    H5Pclose(plist);
    H5Sclose(sid);
  }

  /* Six values, as the reference filter leaves a 1-D chunk's list: compress
     with the seven hdf5lib writes, then decompress with the first six */
  {
    unsigned int cd[7] = {B2_SETTINGS(1, 0), 0, 8, 4096 * 8, 5, 1, 1};
    size_t buf_size = 4096 * sizeof(double);
    void *buf = H5allocate_memory(buf_size, 0);
    if (!buf) { errors++; goto cleanup; }
    memcpy(buf, d, buf_size);
    size_t csize = blosc2_class.filter(0, 7, cd, buf_size, &buf_size, &buf);
    size_t dsize = csize ? blosc2_class.filter(H5Z_FLAG_REVERSE, 6, cd, csize, &buf_size, &buf) : 0;
    if (dsize != 4096 * sizeof(double) || memcmp(buf, d, dsize) != 0) {
      Rprintf("\n      -> Six Blosc2 cd_values did not decompress (got %zu bytes)", dsize);
      errors++;
    }
    H5free_memory(buf);
  }

cleanup:
  if (f) free(f);
  if (f_out) free(f_out);
  if (d) free(d);
  if (d_out) free(d_out);
  if (arr >= 0) H5Tclose(arr);
  return errors ? -1 : 0;
}

/* 24. More ZFP Edge Cases */

/* Chunk dimensions too large for the ZFP header made an optional filter fail
   H5Dcreate2(); a copy of a declined dataset's list failed it too.  The
   compress path trusted a header recorded for another chunk size, and
   compressed a dataset written on a machine of the other byte order as
   though its bytes were native. */
static int test_zfp_edges(hid_t fid) {
  hsize_t big[3] = {70000, 2, 2}, d256[1] = {256}, c256[1] = {256};
  unsigned int vals[16], flags;
  size_t nelmts = 16;
  hid_t sid = -1, plist = -1, dset = -1, dcpl = -1;
  int errors = 0;

  /* 70000 is beyond the 65536 a 3-D ZFP header can hold */
  for (int optional = 1; optional >= 0; optional--) {
    char name[64];
    snprintf(name, sizeof(name), "/zfp_big_%s", optional ? "opt" : "man");
    sid = H5Screate_simple(3, big, NULL);
    plist = H5Pcreate(H5P_DATASET_CREATE);
    H5Pset_chunk(plist, 3, big);
    H5Pset_zfp_accuracy(plist, 0.01);
    if (optional) make_filter_optional(plist, H5Z_FILTER_ZFP);
    dset = H5Dcreate2(fid, name, H5T_NATIVE_DOUBLE, sid, H5P_DEFAULT, plist, H5P_DEFAULT);
    if ((dset >= 0) != optional) {
      Rprintf("\n      -> %s ZFP on a 70000x2x2 chunk: H5Dcreate2() %s", optional ? "Optional" : "Mandatory",
              dset >= 0 ? "succeeded" : "failed");
      errors++;
    }
    if (dset >= 0) H5Dclose(dset);
    H5Pclose(plist);
    H5Sclose(sid);
    dset = plist = sid = -1;
  }

  /* A declined single value's list, re-chunked to a shape ZFP can take */
  if ((dset = H5Dopen2(fid, "/zfp_unsupported_single_opt", H5P_DEFAULT)) >= 0) {
    dcpl = H5Dget_create_plist(dset);
    H5Dclose(dset);
    dset = -1;
  }
  if (dcpl < 0 || H5Pset_chunk(dcpl, 1, c256) < 0) { errors++; goto cleanup; }
  sid = H5Screate_simple(1, d256, NULL);
  if ((dset = H5Dcreate2(fid, "/zfp_copy_declined", H5T_NATIVE_DOUBLE, sid, H5P_DEFAULT, dcpl, H5P_DEFAULT)) < 0) {
    Rprintf("\n      -> Copy of a declined ZFP list failed H5Dcreate2()");
    errors++;
  } else {
    hid_t dcpl2 = H5Dget_create_plist(dset);
    nelmts = 16;
    if (dcpl2 < 0 || H5Pget_filter_by_id(dcpl2, H5Z_FILTER_ZFP, &flags, &nelmts, vals, 0, NULL, NULL) < 0 ||
        nelmts != 2 || vals[1] != 0) {
      Rprintf("\n      -> Copy of a declined ZFP list was not declined (%zu values)", nelmts);
      errors++;
    }
    if (dcpl2 >= 0) H5Pclose(dcpl2);
    H5Dclose(dset);
  }
  dset = -1;
  H5Pclose(dcpl);
  H5Sclose(sid);
  dcpl = sid = -1;

  /* Direct filter calls with the header of a 256-double chunk */
  {
    double in[256], back[256];
    unsigned int hdr[16], swapped[16];
    size_t n = 16;
    for (int i = 0; i < 256; i++) in[i] = sin(i / 10.0) * 10.0;
    if ((dset = H5Dopen2(fid, "/zfp_copy_src_acc", H5P_DEFAULT)) < 0 ||
        (dcpl = H5Dget_create_plist(dset)) < 0 ||
        H5Pget_filter_by_id(dcpl, H5Z_FILTER_ZFP, &flags, &n, hdr, 0, NULL, NULL) < 0) { errors++; goto cleanup; }

    /* A chunk half the header's size must be refused, not over-read */
    size_t buf_size = 128 * sizeof(double);
    void *buf = H5allocate_memory(buf_size, 0);
    memcpy(buf, in, buf_size);
    if (zfp_class.filter(0, n, hdr, buf_size, &buf_size, &buf) != 0) {
      Rprintf("\n      -> ZFP compressed a chunk half the size its header describes");
      errors++;
    }
    H5free_memory(buf);

    /* The same header as a machine of the other byte order stores it, and
       data in that order: compressing and decompressing must round-trip */
    swapped[0] = hdr[0];
    for (size_t k = 1; k < n; k++) {
      unsigned int w = hdr[k];
      swapped[k] = (w >> 24) | ((w >> 8) & 0xFF00u) | ((w << 8) & 0xFF0000u) | (w << 24);
    }
    buf_size = sizeof(in);
    buf = H5allocate_memory(buf_size, 0);
    for (int i = 0; i < 256; i++) {
      unsigned char *src = (unsigned char *)&in[i], *dst = (unsigned char *)buf + i * 8;
      for (int b = 0; b < 8; b++) dst[b] = src[7 - b];
    }
    size_t csize = zfp_class.filter(0, n, swapped, sizeof(in), &buf_size, &buf);
    size_t dsize = csize ? zfp_class.filter(H5Z_FLAG_REVERSE, n, swapped, csize, &buf_size, &buf) : 0;
    int ok = (dsize == sizeof(in));
    for (int i = 0; ok && i < 256; i++) {
      unsigned char *src = (unsigned char *)buf + i * 8, *dst = (unsigned char *)&back[i];
      for (int b = 0; b < 8; b++) dst[b] = src[7 - b];
      if (!(fabs(back[i] - in[i]) <= 0.01)) ok = 0;
    }
    if (!ok) {
      Rprintf("\n      -> ZFP did not round-trip data in the other byte order");
      errors++;
    }
    H5free_memory(buf);
  }

cleanup:
  if (dcpl >= 0) H5Pclose(dcpl);
  if (dset >= 0) H5Dclose(dset);
  if (plist >= 0) H5Pclose(plist);
  if (sid >= 0) H5Sclose(sid);
  return errors ? -1 : 0;
}

/* 25. Bitshuffle Parameters */

/* set_local shifted a copied list (as h5repack makes) once more, so the
   bitshuffle minor version became the codec and every chunk failed.  A block
   size that is not a multiple of 8 failed every chunk instead of
   H5Dcreate2(), and an element size of 0 divided by zero. */
extern const H5Z_class2_t bshuf_class;

static int test_bshuf_params(hid_t fid) {
  hsize_t dims[1] = {4096};
  int32_t *ints = malloc(4096 * sizeof(int32_t)), *ints_out = malloc(4096 * sizeof(int32_t));
  unsigned int cd_lz4[2] = {0, 2}, cd_bad[2] = {12, 2}, vals[16], flags;
  size_t nelmts = 16;
  hid_t sid = -1, plist = -1, dset = -1, dcpl = -1;
  int errors = 0;

  if (!ints || !ints_out) { errors++; goto cleanup; }
  for (int i = 0; i < 4096; i++) ints[i] = i * 3 - 1000;
  CHECK(sid = H5Screate_simple(1, dims, NULL), "H5Screate_simple");

  /* Source dataset, then one created from its stored list */
  CHECK(plist = H5Pcreate(H5P_DATASET_CREATE), "H5Pcreate");
  CHECK(H5Pset_chunk(plist, 1, dims), "H5Pset_chunk");
  CHECK(H5Pset_filter(plist, H5Z_FILTER_BSHUF, H5Z_FLAG_MANDATORY, 2, cd_lz4), "H5Pset_filter");
  CHECK(dset = H5Dcreate2(fid, "/bshuf_src", H5T_NATIVE_INT32, sid, H5P_DEFAULT, plist, H5P_DEFAULT), "H5Dcreate2");
  CHECK(H5Dwrite(dset, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT, ints), "H5Dwrite");
  CHECK(dset = reopen_dset(fid, dset, "/bshuf_src"), "reopen");
  CHECK(dcpl = H5Dget_create_plist(dset), "H5Dget_create_plist");
  H5Dclose(dset);
  dset = -1;
  if ((dset = H5Dcreate2(fid, "/bshuf_copy", H5T_NATIVE_INT32, sid, H5P_DEFAULT, dcpl, H5P_DEFAULT)) < 0 ||
      H5Dwrite(dset, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT, ints) < 0 ||
      (dset = reopen_dset(fid, dset, "/bshuf_copy")) < 0 ||
      H5Dread(dset, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT, ints_out) < 0 ||
      memcmp(ints, ints_out, 4096 * sizeof(int32_t)) != 0) {
    Rprintf("\n      -> Dataset created from a copied Bitshuffle list failed");
    errors++;
  } else {
    hid_t dcpl2 = H5Dget_create_plist(dset);
    if (dcpl2 < 0 || H5Pget_filter_by_id(dcpl2, H5Z_FILTER_BSHUF, &flags, &nelmts, vals, 0, NULL, NULL) < 0 ||
        nelmts < 5 || vals[4] != 2) {
      Rprintf("\n      -> Copied Bitshuffle list lost its codec (%zu values)", nelmts);
      errors++;
    }
    if (dcpl2 >= 0) H5Pclose(dcpl2);
  }
  if (dset >= 0) H5Dclose(dset);
  dset = -1;
  H5Pclose(plist);
  plist = -1;

  /* A block size of 12 is refused at creation */
  CHECK(plist = H5Pcreate(H5P_DATASET_CREATE), "H5Pcreate");
  CHECK(H5Pset_chunk(plist, 1, dims), "H5Pset_chunk");
  CHECK(H5Pset_filter(plist, H5Z_FILTER_BSHUF, H5Z_FLAG_MANDATORY, 2, cd_bad), "H5Pset_filter");
  if ((dset = H5Dcreate2(fid, "/bshuf_block12", H5T_NATIVE_INT32, sid, H5P_DEFAULT, plist, H5P_DEFAULT)) >= 0) {
    Rprintf("\n      -> Bitshuffle accepted a block size of 12");
    errors++;
    H5Dclose(dset);
  }
  dset = -1;

  /* An element size of 0 is refused, not divided by */
  {
    unsigned int cd_zero[5] = {0, 5, 0, 0, 0};
    size_t buf_size = 64;
    void *buf = H5allocate_memory(buf_size, 1);
    if (bshuf_class.filter(H5Z_FLAG_REVERSE, 5, cd_zero, buf_size, &buf_size, &buf) != 0) {
      Rprintf("\n      -> Bitshuffle accepted an element size of 0");
      errors++;
    }
    H5free_memory(buf);
  }

cleanup:
  if (ints) free(ints);
  if (ints_out) free(ints_out);
  if (dcpl >= 0) H5Pclose(dcpl);
  if (dset >= 0) H5Dclose(dset);
  if (plist >= 0) H5Pclose(plist);
  if (sid >= 0) H5Sclose(sid);
  return errors ? -1 : 0;
}

/* 26. A Real Quantizer Filter Keeps Its ID */

/* With a real BitGroom filter registered (as one loaded from HDF5_PLUGIN_PATH
   would be), hdf5lib's read-only shim used to replace it, after which no
   BitGroom dataset could be created.  C_smoke_test() registers this stand-in
   before hdf5lib_register_all_filters(). */
static size_t passthrough_filter(unsigned int flags, size_t cd_nelmts, const unsigned int cd_values[],
                                 size_t nbytes, size_t *buf_size, void **buf) {
  return nbytes;
}

static const H5Z_class2_t real_bitgroom_class = {
  H5Z_CLASS_T_VERS, H5Z_FILTER_BITGROOM, 1, 1, "real_bitgroom_stand_in", NULL, NULL, passthrough_filter };

static int test_shim_defers(hid_t fid) {
  hsize_t dims[1] = {256};
  float data[256], data_out[256];
  hid_t sid = -1, plist = -1, dset = -1;
  int ret = -1;

  for (int i = 0; i < 256; i++) data[i] = (float)i * 0.25f;
  CHECK(sid = H5Screate_simple(1, dims, NULL), "H5Screate_simple");
  CHECK(plist = H5Pcreate(H5P_DATASET_CREATE), "H5Pcreate");
  CHECK(H5Pset_chunk(plist, 1, dims), "H5Pset_chunk");
  CHECK(H5Pset_filter(plist, H5Z_FILTER_BITGROOM, H5Z_FLAG_MANDATORY, 0, NULL), "H5Pset_filter");
  CHECK(dset = H5Dcreate2(fid, "/bitgroom_real", H5T_NATIVE_FLOAT, sid, H5P_DEFAULT, plist, H5P_DEFAULT), "H5Dcreate2");
  CHECK(H5Dwrite(dset, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, H5P_DEFAULT, data), "H5Dwrite");
  CHECK(dset = reopen_dset(fid, dset, "/bitgroom_real"), "reopen");
  CHECK(H5Dread(dset, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, H5P_DEFAULT, data_out), "H5Dread");
  if (memcmp(data, data_out, sizeof(data)) != 0) {
    Rprintf("\n      -> BitGroom stand-in did not round-trip");
    goto cleanup;
  }
  ret = 0;

cleanup:
  CLOSE_DSET(dset);
  if (plist >= 0) H5Pclose(plist);
  if (sid >= 0) H5Sclose(sid);
  return ret;
}

/* 27. Tear-Down Unregisters Every Filter It Registered */
static int test_filters_unregistered(void) {
  H5Z_filter_t ids[] = {H5Z_FILTER_BLOSC, H5Z_FILTER_BLOSC2, H5Z_FILTER_BSHUF, H5Z_FILTER_BZIP2,
                        H5Z_FILTER_LZ4, H5Z_FILTER_LZF, H5Z_FILTER_SNAPPY, H5Z_FILTER_ZFP,
                        H5Z_FILTER_ZSTD, H5Z_FILTER_GRANULARBR, H5Z_FILTER_BITROUND};
  unsigned int state;
  int ret = 0;

  /* Look in HDF5's filter table only, not in the plugin directories */
  if (H5PLget_loading_state(&state) < 0 || H5PLset_loading_state(0) < 0) return -1;
  for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
    if (H5Zfilter_avail(ids[i]) != 0) {
      Rprintf("\n      -> Filter %d is still registered", (int)ids[i]);
      ret = -1;
    }
  }
  /* The stand-in BitGroom filter was not hdf5lib's to remove */
  if (H5Zfilter_avail(H5Z_FILTER_BITGROOM) <= 0) {
    Rprintf("\n      -> Tear-down removed a BitGroom filter hdf5lib did not register");
    ret = -1;
  }
  H5Zunregister(H5Z_FILTER_BITGROOM);
  H5PLset_loading_state(state);
  return ret;
}

/* 28. Snappy Chunks in snappy-cuda's Layout */

/* ID 32003 is registered to snappy-cuda, whose chunks hold a varint32 length,
   a varint32 block size, then each block as a 4-byte little-endian length and
   its Snappy elements.  hdf5lib used to write one raw Snappy stream per chunk,
   which snappy-cuda can't read.  It now writes 32 KiB blocks, all full but the
   last (snappy-cuda's GPU decoder relies on that), and still reads raw chunks,
   including ones that happen to parse as blocks.  csnappy is in libhdf5z.a. */
void csnappy_compress(const char *input, uint32_t input_length, char *compressed,
                      uint32_t *out_compressed_length, void *working_memory, const int workmem_bytes_power_of_two);
int csnappy_decompress_noheader(const char *src, uint32_t src_len, char *dst, uint32_t *dst_len);

#define SNAPPY_N 10000  /* doubles: 80000 bytes, so blocks of 32768, 32768 and 14464 */

static size_t snappy_varint(const uint8_t *p, size_t n, uint32_t *val) {
  *val = 0;
  for (size_t i = 0; i < 5 && i < n; i++) {
    *val |= (uint32_t)(p[i] & 0x7F) << (7 * i);
    if (!(p[i] & 0x80)) return i + 1;
  }
  return 0;
}

static uint32_t snappy_le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Stores chunk[0..size) as the only chunk of a new 1-D Snappy dataset of n
   items, then reads it back through a fresh handle.  Returns H5Dread()'s
   result, or -1 if the dataset could not be set up. */
static herr_t snappy_read_chunk(hid_t fid, const char *name, hid_t type, hsize_t n,
                                const void *chunk, size_t size, void *out) {
  hsize_t dims[1] = {n}, offset[1] = {0};
  hid_t sid = -1, plist = -1, dset = -1;
  herr_t ret = -1;

  CHECK(sid = H5Screate_simple(1, dims, NULL), "H5Screate_simple");
  CHECK(plist = H5Pcreate(H5P_DATASET_CREATE), "H5Pcreate");
  CHECK(H5Pset_chunk(plist, 1, dims), "H5Pset_chunk");
  CHECK(H5Pset_filter(plist, H5Z_FILTER_SNAPPY, H5Z_FLAG_MANDATORY, 0, NULL), "H5Pset_filter");
  CHECK(dset = H5Dcreate2(fid, name, type, sid, H5P_DEFAULT, plist, H5P_DEFAULT), "H5Dcreate2");
  CHECK(H5Dwrite_chunk(dset, H5P_DEFAULT, 0, offset, size, chunk), "H5Dwrite_chunk");
  CHECK(dset = reopen_dset(fid, dset, name), "reopen");
  ret = H5Dread(dset, type, H5S_ALL, H5S_ALL, H5P_DEFAULT, out);

cleanup:
  if (dset >= 0) H5Dclose(dset);
  if (plist >= 0) H5Pclose(plist);
  if (sid >= 0) H5Sclose(sid);
  return ret;
}

/* Long runs of equal values, so that the raw stream ends in a copy */
static void snappy_data(double *data) {
  for (int i = 0; i < SNAPPY_N; i++) data[i] = (double)(i / 64) + (i / 64 % 3) * 0.25;
}

/* (a) Written chunks are in snappy-cuda's layout, and (d) cut or altered
   copies of one fail to read */
static int test_snappy_written(hid_t fid) {
  hsize_t dims[1] = {SNAPPY_N}, offset[1] = {0}, size = 0;
  double *data = malloc(SNAPPY_N * sizeof(double)), *data_out = malloc(SNAPPY_N * sizeof(double));
  uint8_t *raw = NULL, *bad = NULL;
  uint32_t mask = 0, total = 0, block = 0;
  hid_t sid = -1, plist = -1, dset = -1;
  int ret = -1;

  if (!data || !data_out) goto cleanup;
  snappy_data(data);

  CHECK(sid = H5Screate_simple(1, dims, NULL), "H5Screate_simple");
  CHECK(plist = H5Pcreate(H5P_DATASET_CREATE), "H5Pcreate");
  CHECK(H5Pset_chunk(plist, 1, dims), "H5Pset_chunk");
  CHECK(H5Pset_filter(plist, H5Z_FILTER_SNAPPY, H5Z_FLAG_MANDATORY, 0, NULL), "H5Pset_filter");
  CHECK(dset = H5Dcreate2(fid, "/snappy_layout", H5T_NATIVE_DOUBLE, sid, H5P_DEFAULT, plist, H5P_DEFAULT), "H5Dcreate2");
  CHECK(H5Dwrite(dset, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, data), "H5Dwrite");
  CHECK(dset = reopen_dset(fid, dset, "/snappy_layout"), "reopen");
  CHECK(H5Dget_chunk_storage_size(dset, offset, &size), "H5Dget_chunk_storage_size");
  if (size < 8 || !(raw = malloc((size_t)size)) || !(bad = malloc((size_t)size))) goto cleanup;
  size_t n = (size_t)size;
  CHECK(H5Dread_chunk(dset, H5P_DEFAULT, offset, &mask, raw, &n), "H5Dread_chunk");

  size_t a = snappy_varint(raw, n, &total), b = snappy_varint(raw + a, n - a, &block);
  if (!a || !b || total != SNAPPY_N * sizeof(double) || block != 32768) {
    Rprintf("\n      -> Chunk starts with length %u and block size %u, not %u and 32768",
            total, block, (unsigned)(SNAPPY_N * sizeof(double)));
    goto cleanup;
  }
  size_t pos = a + b;
  for (uint32_t done = 0; done < total; done += block) {
    uint32_t want = (total - done < block) ? total - done : block, got = want;
    if (n - pos < 4 || snappy_le32(raw + pos) > n - pos - 4 ||
        csnappy_decompress_noheader((const char *)raw + pos + 4, snappy_le32(raw + pos),
                                    (char *)data_out + done, &got) != 0 || got != want) {
      Rprintf("\n      -> Block at byte %u of the chunk is not %u bytes of Snappy", done, want);
      goto cleanup;
    }
    pos += 4 + snappy_le32(raw + pos);
  }
  if (pos != n || memcmp(data, data_out, total) != 0) {
    Rprintf("\n      -> Blocks end at byte %zu of %zu, or decode to other values", pos, n);
    goto cleanup;
  }

  /* Missing its last byte; a block size of 32767 (same varint length); a
     total of one byte more; the first block's length one byte longer */
  for (int k = 0; k < 4; k++) {
    char name[32];
    size_t len = (k == 0) ? n - 1 : n;
    memcpy(bad, raw, n);
    if (k == 1) { bad[a] = 0xFF; bad[a + 1] = 0xFF; bad[a + 2] = 0x01; }
    if (k == 2) bad[0] += 1;
    if (k == 3) bad[a + b] += 1;
    snprintf(name, sizeof(name), "/snappy_bad%d", k);
    if (snappy_read_chunk(fid, name, H5T_NATIVE_DOUBLE, SNAPPY_N, bad, len, data_out) >= 0) {
      Rprintf("\n      -> Altered chunk %d read without error", k);
      goto cleanup;
    }
  }
  ret = 0;

cleanup:
  if (data) free(data);
  if (data_out) free(data_out);
  if (raw) free(raw);
  if (bad) free(bad);
  CLOSE_DSET(dset);
  if (plist >= 0) H5Pclose(plist);
  if (sid >= 0) H5Sclose(sid);
  return ret;
}

/* (b) Raw Snappy chunks, as older hdf5lib wrote them, still read back,
   unless cut short by even a byte */
static int test_snappy_raw(hid_t fid) {
  double *data = malloc(SNAPPY_N * sizeof(double)), *data_out = malloc(SNAPPY_N * sizeof(double));
  uint32_t total = SNAPPY_N * sizeof(double), len = 0;
  char *raw = malloc(32 + total + total / 6), *work = malloc(1 << 15);
  int ret = -1;

  if (!data || !data_out || !raw || !work) goto cleanup;
  snappy_data(data);
  csnappy_compress((const char *)data, total, raw, &len, work, 15);

  if (snappy_read_chunk(fid, "/snappy_raw", H5T_NATIVE_DOUBLE, SNAPPY_N, raw, len, data_out) < 0 ||
      memcmp(data, data_out, total) != 0) {
    Rprintf("\n      -> Raw Snappy chunk did not read back");
    goto cleanup;
  }
  for (uint32_t cut = 1; cut <= 2; cut++) {
    char name[32];
    snprintf(name, sizeof(name), "/snappy_raw_cut%u", cut);
    if (snappy_read_chunk(fid, name, H5T_NATIVE_DOUBLE, SNAPPY_N, raw, len - cut, data_out) >= 0) {
      Rprintf("\n      -> Raw Snappy chunk missing %u byte(s) read without error", cut);
      goto cleanup;
    }
  }

  /* Four zero bytes compress to 04 0C 00 00 00 00, which also parses as a
     total of 4 in blocks of 12, with one empty block */
  int32_t zero = 0, zero_out = -1;
  csnappy_compress((const char *)&zero, sizeof(zero), raw, &len, work, 9);
  if (len != 6 || snappy_read_chunk(fid, "/snappy_raw_zero", H5T_NATIVE_INT32, 1, raw, len, &zero_out) < 0 ||
      zero_out != 0) {
    Rprintf("\n      -> Raw Snappy chunk of one zero int32 did not read back");
    goto cleanup;
  }
  ret = 0;

cleanup:
  if (data) free(data);
  if (data_out) free(data_out);
  if (raw) free(raw);
  if (work) free(work);
  return ret;
}

/* (c) A chunk built by hand as snappy-cuda lays them out, with a block size of
   1000: one literal; then a 40-byte literal and copies of it; then the same
   in the 500 bytes left */
static int test_snappy_cuda_chunk(hid_t fid) {
  uint8_t data[2500], data_out[2500], chunk[2600], *p = chunk;
  int ret = -1;

  for (int i = 0; i < 2500; i++) data[i] = (uint8_t)((i % 40) * 7 + i / 1000);
  *p++ = 0xC4; *p++ = 0x13;  /* 2500 */
  *p++ = 0xE8; *p++ = 0x07;  /* 1000 */
  for (int start = 0; start < 2500; start += 1000) {
    int len = (2500 - start < 1000) ? 2500 - start : 1000;
    uint8_t *size_at = p;
    p += 4;
    if (start == 0) {
      *p++ = 61 << 2; *p++ = (uint8_t)((len - 1) & 0xFF); *p++ = (uint8_t)((len - 1) >> 8);
      memcpy(p, data, len); p += len;
    } else {
      *p++ = 39 << 2;
      memcpy(p, data + start, 40); p += 40;
      for (int left = len - 40; left > 0; left -= 64) {
        int copy = (left < 64) ? left : 64;
        *p++ = (uint8_t)(2 | ((copy - 1) << 2)); *p++ = 40; *p++ = 0;
      }
    }
    uint32_t block_len = (uint32_t)(p - size_at - 4);
    size_at[0] = (uint8_t)block_len; size_at[1] = (uint8_t)(block_len >> 8);
    size_at[2] = (uint8_t)(block_len >> 16); size_at[3] = (uint8_t)(block_len >> 24);
  }

  if (snappy_read_chunk(fid, "/snappy_cuda", H5T_NATIVE_UCHAR, 2500, chunk, (size_t)(p - chunk), data_out) < 0 ||
      memcmp(data, data_out, sizeof(data)) != 0) {
    Rprintf("\n      -> Hand-built snappy-cuda chunk did not read back");
    goto cleanup;
  }
  ret = 0;

cleanup:
  return ret;
}

/* 29. Blosc2's Stored cd_values, Slot by Slot */

/* Every slot holds what the reference filter keeps there, and hdf5lib's
   pre-filter bitmask and meta value live in slot 0 (see CD_VALUES in
   blosc2_plugin.c).  Section 13 checks 1-D, 2-D and 17-D chunks; these are an
   array datatype, the codecs that skip the pre-filters, and pipelines. */
#define B2_N 4096  /* items in each dataset below, all in one chunk */

enum { B2_F32, B2_F64, B2_I32, B2_ARR };

typedef struct {
  hid_t type[4];
  const void *data[4];
  float *f32;
  double *f64;
  int32_t *i32;
  unsigned char *out;
} b2_data_t;

static int b2_data_init(b2_data_t *d) {
  hsize_t adims[1] = {4};
  d->f32 = malloc(B2_N * 4 * sizeof(float));
  d->f64 = malloc(B2_N * sizeof(double));
  d->i32 = malloc(B2_N * sizeof(int32_t));
  d->out = malloc(B2_N * 4 * sizeof(float));
  d->type[B2_ARR] = H5Tarray_create2(H5T_NATIVE_FLOAT, 1, adims);
  if (!d->f32 || !d->f64 || !d->i32 || !d->out || d->type[B2_ARR] < 0) return -1;
  for (int i = 0; i < B2_N * 4; i++) d->f32[i] = (float)(sin(i / 20.0) * 1000.0);
  for (int i = 0; i < B2_N; i++) { d->f64[i] = sin(i / 20.0) * 10.0; d->i32[i] = i % 1000 + i / 7; }
  d->type[B2_F32] = H5T_NATIVE_FLOAT;  d->data[B2_F32] = d->f32;
  d->type[B2_F64] = H5T_NATIVE_DOUBLE; d->data[B2_F64] = d->f64;
  d->type[B2_I32] = H5T_NATIVE_INT32;  d->data[B2_I32] = d->i32;
  d->data[B2_ARR] = d->f32;
  return 0;
}

static void b2_data_free(b2_data_t *d) {
  if (d->f32) free(d->f32);
  if (d->f64) free(d->f64);
  if (d->i32) free(d->i32);
  if (d->out) free(d->out);
  if (d->type[B2_ARR] >= 0) H5Tclose(d->type[B2_ARR]);
}

/* Writes B2_N items as one chunk of a dataset made from the Blosc2 list cd,
   1-D or 64 x 64, and reads them into out through a fresh handle.  Hands back
   the stored list and, if raw is given, the chunk as stored (to free).
   Returns -2 if H5Dcreate2() refused, or -1 on any other failure. */
static int b2_store(hid_t fid, const char *name, hid_t type, int rank, const unsigned int *cd, size_t ncd,
                    const void *data, void *out, unsigned int *vals, size_t *nelmts,
                    unsigned char **raw, size_t *raw_size) {
  hsize_t chunk[2] = {B2_N, 1}, offset[2] = {0, 0}, size = 0;
  if (rank == 2) chunk[0] = chunk[1] = 64;
  uint32_t mask = 0;
  unsigned int flags;
  hid_t sid = -1, plist = -1, dset = -1, dcpl = -1;
  int ret = -1;

  CHECK(sid = H5Screate_simple(rank, chunk, NULL), "H5Screate_simple");
  CHECK(plist = H5Pcreate(H5P_DATASET_CREATE), "H5Pcreate");
  CHECK(H5Pset_chunk(plist, rank, chunk), "H5Pset_chunk");
  CHECK(H5Pset_filter(plist, H5Z_FILTER_BLOSC2, H5Z_FLAG_MANDATORY, ncd, cd), "H5Pset_filter");
  if ((dset = H5Dcreate2(fid, name, type, sid, H5P_DEFAULT, plist, H5P_DEFAULT)) < 0) {
    ret = -2;
    goto cleanup;
  }
  CHECK(H5Dwrite(dset, type, H5S_ALL, H5S_ALL, H5P_DEFAULT, data), "H5Dwrite");
  CHECK(dset = reopen_dset(fid, dset, name), "reopen");
  CHECK(H5Dread(dset, type, H5S_ALL, H5S_ALL, H5P_DEFAULT, out), "H5Dread");
  CHECK(dcpl = H5Dget_create_plist(dset), "H5Dget_create_plist");
  *nelmts = 32;
  CHECK(H5Pget_filter_by_id(dcpl, H5Z_FILTER_BLOSC2, &flags, nelmts, vals, 0, NULL, NULL), "H5Pget_filter_by_id");
  if (raw) {
    CHECK(H5Dget_chunk_storage_size(dset, offset, &size), "H5Dget_chunk_storage_size");
    if (!(*raw = malloc((size_t)size))) goto cleanup;
    *raw_size = (size_t)size;
    CHECK(H5Dread_chunk(dset, H5P_DEFAULT, offset, &mask, *raw, raw_size), "H5Dread_chunk");
  }
  ret = 0;

cleanup:
  if (dcpl >= 0) H5Pclose(dcpl);
  CLOSE_DSET(dset);
  if (plist >= 0) H5Pclose(plist);
  if (sid >= 0) H5Sclose(sid);
  return ret;
}

static int test_blosc2_lists(hid_t fid) {
  struct {
    const char *name; int type, rank, lossless; unsigned int cd[7]; size_t nwant; unsigned int want[10];
  } cases[] = {
    /* An array datatype: its items are the floats, which the chunk shape does not count */
    {"/b2list_array", B2_ARR, 2, 1, {B2_SETTINGS(2, 0), 0, 0, 0, 5, 2, 1},
     7, {B2_SETTINGS(2, 0), 0, 4, B2_N * 16, 5, 2, 1}},
    /* NDLZ skips shuffle; it sees a byte image of 64 rows of 64 floats */
    {"/b2list_ndlz_2d", B2_F32, 2, 1, {B2_SETTINGS(1, 8), 0, 0, 0, 5, 1, 32},
     10, {B2_SETTINGS(0, 8), 0, 1, B2_N * 4, 5, 0, 32, 2, 64, 256}},
    /* 1-D int32 too, with its cell size left at 4 */
    {"/b2list_ndlz_1d", B2_I32, 1, 1, {B2_SETTINGS(0, 0), 0, 0, 0, 5, 0, 32},
     10, {B2_SETTINGS(0, 4), 0, 1, B2_N * 4, 5, 0, 32, 2, B2_N, 4}},
    /* ZFP accuracy 10^-3 skips the pre-filters; 1-D chunks go without geometry */
    {"/b2list_zfp_2d", B2_F64, 2, 0, {B2_SETTINGS(0, 253), 0, 0, 0, 5, 0, 33},
     10, {B2_SETTINGS(0, 253), 0, 8, B2_N * 8, 5, 0, 33, 2, 64, 64}},
    {"/b2list_zfp_1d", B2_F64, 1, 0, {B2_SETTINGS(1, 253), 0, 0, 0, 5, 1, 33},
     7, {B2_SETTINGS(0, 253), 0, 8, B2_N * 8, 5, 0, 33}},
    /* Delta alone; a meta value nothing reads is not kept */
    {"/b2list_delta", B2_I32, 1, 1, {B2_SETTINGS(4, 77), 0, 0, 0, 5, 3, 5},
     7, {B2_SETTINGS(4, 0), 0, 4, B2_N * 4, 5, 3, 5}},
    /* Trunc-prec alone, keeping 10 bits */
    {"/b2list_trunc", B2_F32, 1, 0, {B2_SETTINGS(8, 10), 0, 0, 0, 5, 4, 1},
     7, {B2_SETTINGS(8, 10), 0, 4, B2_N * 4, 5, 4, 1}},
    /* Trunc-prec dropping 12 bits, delta, then shuffle */
    {"/b2list_tds", B2_F32, 2, 0, {B2_SETTINGS(13, -12), 0, 0, 0, 5, 1, 4},
     10, {B2_SETTINGS(13, -12), 0, 4, B2_N * 4, 5, 1, 4, 2, 64, 64}},
    /* All four: bitshuffle takes shuffle's place and runs last; slot 5 left 0 */
    {"/b2list_all", B2_F64, 2, 0, {B2_SETTINGS(15, 20), 0, 0, 0, 9, 0, 5},
     10, {B2_SETTINGS(14, 20), 0, 8, B2_N * 8, 9, 2, 5, 2, 64, 64}},
  };
  b2_data_t d;
  unsigned int vals[32];
  size_t nelmts;
  int errors = 0;

  if (b2_data_init(&d) < 0) { errors++; goto cleanup; }
  for (size_t k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
    int t = cases[k].type;
    if (b2_store(fid, cases[k].name, d.type[t], cases[k].rank, cases[k].cd, 7, d.data[t], d.out,
                 vals, &nelmts, NULL, NULL) < 0) {
      Rprintf("\n      -> %s: could not be written and read", cases[k].name);
      errors++;
    } else if (!b2_list_is(vals, nelmts, cases[k].want, cases[k].nwant)) {
      b2_print_list(cases[k].name, vals, nelmts);
      errors++;
    } else if (cases[k].lossless && memcmp(d.data[t], d.out, B2_N * H5Tget_size(d.type[t])) != 0) {
      Rprintf("\n      -> %s: values changed", cases[k].name);
      errors++;
    }
  }

cleanup:
  b2_data_free(&d);
  return errors ? -1 : 0;
}

/* 30. Blosc2 Lists in Other Forms */

/* Each list below must store the same values as the new form beside it, and
   write the same chunk: hdf5lib's earlier input (cd_values[0] = 0, slot 5 the
   bitmask, an 8th value the meta value), the reference filter's (1, slot 5
   one filter code), and lists earlier versions stored (2), as a copied
   creation property list hands them over.  Compressing with an old stored
   list directly, as when appending to an old dataset, must also write that
   same chunk. */
static int test_blosc2_input_forms(hid_t fid) {
  struct {
    const char *name; int type, rank; size_t n; unsigned int cd[11]; unsigned int new_cd[7];
  } forms[] = {
    /* Trunc-prec(8) + bitshuffle, lz4, as callers gave it */
    {"old_trunc_1d", B2_F32, 1, 8, {0, 0, 0, 0, 5, 10, 1, 8}, {B2_SETTINGS(10, 8), 0, 0, 0, 5, 2, 1}},
    {"old_trunc_2d", B2_F32, 2, 8, {0, 0, 0, 0, 5, 10, 1, 8}, {B2_SETTINGS(10, 8), 0, 0, 0, 5, 2, 1}},
    /* hdf5plugin's 3 (delta) is still the bitmask 1 | 2 here: bitshuffle */
    {"old_three",    B2_I32, 1, 7, {0, 0, 0, 0, 5, 3, 5}, {B2_SETTINGS(2, 0), 0, 0, 0, 5, 2, 5}},
    {"old_delta_sh", B2_I32, 2, 7, {0, 0, 0, 0, 5, 5, 4}, {B2_SETTINGS(5, 0), 0, 0, 0, 5, 1, 4}},
    {"old_ndlz",     B2_I32, 2, 8, {0, 0, 0, 0, 5, 0, 32, 8}, {B2_SETTINGS(0, 8), 0, 0, 0, 5, 0, 32}},
    {"old_zfp",      B2_F64, 2, 8, {0, 0, 0, 0, 5, 0, 34, 20}, {B2_SETTINGS(0, 20), 0, 0, 0, 5, 0, 34}},
    /* No values at all: level 5, shuffle, BloscLZ */
    {"old_none",     B2_F64, 1, 0, {0}, {B2_SETTINGS(1, 0), 0, 0, 0, 5, 1, 0}},
    /* The reference filter's: 3 is delta */
    {"ref_delta",    B2_I32, 1, 7, {1, 0, 4, B2_N * 4, 5, 3, 1}, {B2_SETTINGS(4, 0), 0, 0, 0, 5, 3, 1}},
    /* As it stores a 2-D chunk's list (a DCPL copied from h5py) */
    {"ref_2d",       B2_F32, 2, 10, {1, 0, 4, B2_N * 4, 5, 2, 1, 2, 64, 64}, {B2_SETTINGS(2, 0), 0, 0, 0, 5, 2, 1}},
    /* Four values: its defaults, shuffle and BloscLZ at level 5 */
    {"ref_short",    B2_F64, 1, 4, {1, 0, 0, 0}, {B2_SETTINGS(1, 0), 0, 0, 0, 5, 1, 0}},
    /* Stored by earlier versions: the meta value in slot 5's next byte (1-D) */
    {"rev2_1d",      B2_F32, 1, 7, {2, 0, 4, B2_N * 4, 5, 10 | 8 << 8, 1}, {B2_SETTINGS(10, 8), 0, 0, 0, 5, 2, 1}},
    /* ... after a rank of 1 and its dimension (hdf5lib 2.2.0.0) */
    {"rev2_rank1",   B2_F32, 1, 10, {2, 0, 4, B2_N * 4, 5, 10, 1, 1, B2_N, 8}, {B2_SETTINGS(10, 8), 0, 0, 0, 5, 2, 1}},
    /* ... or after the B2ND shape */
    {"rev2_2d",      B2_F32, 2, 11, {2, 0, 4, B2_N * 4, 5, 10, 1, 2, 64, 64, 8}, {B2_SETTINGS(10, 8), 0, 0, 0, 5, 2, 1}},
    {"rev2_zfp_2d",  B2_F64, 2, 11, {2, 0, 8, B2_N * 8, 5, 0, 33, 2, 64, 64, 253}, {B2_SETTINGS(0, 253), 0, 0, 0, 5, 0, 33}},
  };
  b2_data_t d;
  unsigned char *out_new = malloc(B2_N * 16), *raw_old = NULL, *raw_new = NULL;
  unsigned int vals_old[32], vals_new[32];
  size_t n_old = 0, n_new = 0, size_old = 0, size_new = 0;
  int errors = 0, old_nthreads = hdf5lib_set_blosc_nthreads(1);

  if (b2_data_init(&d) < 0 || !out_new) { errors++; goto cleanup; }
  for (size_t k = 0; k < sizeof(forms) / sizeof(forms[0]); k++) {
    int t = forms[k].type;
    size_t nbytes = B2_N * H5Tget_size(d.type[t]);
    char name_old[64], name_new[64];
    snprintf(name_old, sizeof(name_old), "/b2form_%s", forms[k].name);
    snprintf(name_new, sizeof(name_new), "/b2form_%s_new", forms[k].name);

    if (raw_old) { free(raw_old); raw_old = NULL; }
    if (raw_new) { free(raw_new); raw_new = NULL; }
    if (b2_store(fid, name_old, d.type[t], forms[k].rank, forms[k].cd, forms[k].n, d.data[t], d.out,
                 vals_old, &n_old, &raw_old, &size_old) < 0 ||
        b2_store(fid, name_new, d.type[t], forms[k].rank, forms[k].new_cd, 7, d.data[t], out_new,
                 vals_new, &n_new, &raw_new, &size_new) < 0) {
      Rprintf("\n      -> %s: could not be written and read", forms[k].name);
      errors++;
      continue;
    }
    if (!b2_list_is(vals_old, n_old, vals_new, n_new)) {
      b2_print_list(name_old, vals_old, n_old);
      b2_print_list(name_new, vals_new, n_new);
      errors++;
      continue;
    }
    if (size_old != size_new || memcmp(raw_old, raw_new, size_old) != 0 || memcmp(d.out, out_new, nbytes) != 0) {
      Rprintf("\n      -> %s: chunk of %zu bytes, not the new form's %zu, or other values", forms[k].name,
              size_old, size_new);
      errors++;
      continue;
    }

    /* A stored list (not a caller's) straight to the filter, both ways */
    if (forms[k].cd[0] == 0 || forms[k].n < 7) continue;
    size_t buf_size = nbytes;
    void *buf = H5allocate_memory(buf_size, 0);
    if (!buf) { errors++; continue; }
    memcpy(buf, d.data[t], nbytes);
    size_t csize = blosc2_class.filter(0, forms[k].n, forms[k].cd, nbytes, &buf_size, &buf);
    int same = (csize == size_new && memcmp(buf, raw_new, csize) == 0);
    size_t dsize = csize ? blosc2_class.filter(H5Z_FLAG_REVERSE, forms[k].n, forms[k].cd, csize, &buf_size, &buf) : 0;
    if (!same || dsize != nbytes || memcmp(buf, out_new, nbytes) != 0) {
      Rprintf("\n      -> %s: the filter wrote %zu bytes from the list directly (%s the new form's chunk)",
              forms[k].name, csize, same ? "same as" : "not");
      errors++;
    }
    H5free_memory(buf);
  }

cleanup:
  hdf5lib_set_blosc_nthreads(old_nthreads);
  if (out_new) free(out_new);
  if (raw_old) free(raw_old);
  if (raw_new) free(raw_new);
  b2_data_free(&d);
  return errors ? -1 : 0;
}

/* 31. Blosc2 Lists hdf5lib Refuses */

/* H5Dcreate2() must refuse a slot 5 the bitmask in slot 0 contradicts, an
   unknown slot 0, a layout this version does not know, and reference lists
   asking for what hdf5lib cannot run.  The filter must refuse to compress
   with the last two -- such lists can only come from a file -- but read
   chunks whatever the list says, as every chunk records its own settings. */
static int test_blosc2_refused(hid_t fid) {
  const unsigned int layout2 = (0xB2u << 24) | (2u << 20) | (1u << 16) | 0x01u;
  struct { const char *name; unsigned int cd[7]; } bad[] = {
    {"/b2bad_slot5",     {B2_SETTINGS(2, 0), 0, 0, 0, 5, 1, 1}},    /* bitshuffle runs last, not shuffle */
    {"/b2bad_order",     {B2_SETTINGS(12, 10), 0, 0, 0, 5, 4, 1}},  /* delta runs after trunc-prec */
    {"/b2bad_rev3",      {3, 0, 0, 0, 5, 1, 1}},
    {"/b2bad_rev_other", {0x12345678u, 0, 0, 0, 5, 1, 1}},
    {"/b2bad_layout2",   {layout2, 0, 0, 0, 5, 1, 1}},
    {"/b2bad_ref_code",  {1, 0, 0, 0, 5, 5, 1}},                    /* not a pre-filter hdf5lib runs */
    {"/b2bad_ref_trunc", {1, 0, 0, 0, 5, 4, 1}},                    /* trunc-prec, but no precision */
  };
  b2_data_t d;
  unsigned int vals[32];
  size_t nelmts, nbytes = B2_N * sizeof(float);
  int errors = 0;

  if (b2_data_init(&d) < 0) { errors++; goto cleanup; }
  for (size_t k = 0; k < sizeof(bad) / sizeof(bad[0]); k++) {
    if (b2_store(fid, bad[k].name, H5T_NATIVE_FLOAT, 1, bad[k].cd, 7, d.f32, d.out, vals, &nelmts, NULL, NULL) != -2) {
      Rprintf("\n      -> %s: H5Dcreate2() accepted it", bad[k].name);
      errors++;
    }
  }

  /* Compress one chunk properly, then hand the filter lists it cannot use */
  unsigned int good[7] = {B2_SETTINGS(1, 0), 0, 4, (unsigned int)nbytes, 5, 1, 1};
  unsigned int from_file[2][7] = {{layout2, 0, 4, (unsigned int)nbytes, 5, 1, 1},
                                  {1, 0, 4, (unsigned int)nbytes, 5, 7, 1}};
  size_t buf_size = nbytes;
  void *buf = H5allocate_memory(buf_size, 0);
  if (!buf) { errors++; goto cleanup; }
  memcpy(buf, d.f32, nbytes);
  size_t csize = blosc2_class.filter(0, 7, good, nbytes, &buf_size, &buf);
  if (csize == 0) { Rprintf("\n      -> The filter did not compress"); errors++; }

  for (int k = 0; csize && k < 2; k++) {
    size_t probe_size = nbytes;
    void *probe = H5allocate_memory(probe_size, 0);
    if (!probe) { errors++; break; }
    memcpy(probe, d.f32, nbytes);
    if (blosc2_class.filter(0, 7, from_file[k], nbytes, &probe_size, &probe) != 0) {
      Rprintf("\n      -> The filter compressed with slot 0 = 0x%08X, slot 5 = %u", from_file[k][0], from_file[k][5]);
      errors++;
    }
    H5free_memory(probe);

    probe_size = csize;
    if (!(probe = H5allocate_memory(probe_size, 0))) { errors++; break; }
    memcpy(probe, buf, csize);
    size_t dsize = blosc2_class.filter(H5Z_FLAG_REVERSE, 7, from_file[k], csize, &probe_size, &probe);
    if (dsize != nbytes || memcmp(probe, d.f32, nbytes) != 0) {
      Rprintf("\n      -> A chunk did not read back with slot 0 = 0x%08X, slot 5 = %u", from_file[k][0], from_file[k][5]);
      errors++;
    }
    H5free_memory(probe);
  }
  H5free_memory(buf);

cleanup:
  b2_data_free(&d);
  return errors ? -1 : 0;
}

/* 32. Blosc2's Setter */

/* hdf5lib_set_blosc2() must put in the property list the list the compression
   article spells out, which H5Dcreate2() then takes; update a Blosc2 filter
   already there in place; and refuse what slot 0 cannot hold, adding nothing. */
static int test_blosc2_setter(hid_t fid) {
  struct {
    const char *name; int type; unsigned int codec, level, prefilters; int meta; unsigned int want[7];
  } cases[] = {
    /* The compression article's examples */
    {"/b2set_bitshuffle", B2_I32, HDF5LIB_BLOSC2_ZSTD, 5, HDF5LIB_BLOSC2_BITSHUFFLE, 0,
     {0xB2120001u, 0, 0, 0, 5, 2, 5}},
    {"/b2set_pipeline",   B2_I32, HDF5LIB_BLOSC2_ZSTD, 5, HDF5LIB_BLOSC2_DELTA | HDF5LIB_BLOSC2_BITSHUFFLE, 0,
     {0xB2160001u, 0, 0, 0, 5, 2, 5}},
    {"/b2set_trunc",      B2_F32, HDF5LIB_BLOSC2_LZ4, 5, HDF5LIB_BLOSC2_TRUNC_PREC | HDF5LIB_BLOSC2_BITSHUFFLE, 16,
     {0xB21A1001u, 0, 0, 0, 5, 2, 1}},
    {"/b2set_zfp_prec",   B2_F64, HDF5LIB_BLOSC2_ZFP_PREC, 5, 0, 16, {0xB2101001u, 0, 0, 0, 5, 0, 34}},
    {"/b2set_zfp_acc",    B2_F64, HDF5LIB_BLOSC2_ZFP_ACC,  5, 0, -3, {0xB210FD01u, 0, 0, 0, 5, 0, 33}},
    {"/b2set_zfp_rate",   B2_F32, HDF5LIB_BLOSC2_ZFP_RATE, 5, 0, 25, {0xB2101901u, 0, 0, 0, 5, 0, 35}},
    /* The other codecs, and each other pre-filter as the last to run */
    {"/b2set_blosclz",    B2_I32, HDF5LIB_BLOSC2_BLOSCLZ, 1, HDF5LIB_BLOSC2_SHUFFLE, 0,
     {B2_SETTINGS(1, 0), 0, 0, 0, 1, 1, 0}},
    {"/b2set_lz4hc",      B2_I32, HDF5LIB_BLOSC2_LZ4HC, 9, HDF5LIB_BLOSC2_DELTA, 0,
     {B2_SETTINGS(4, 0), 0, 0, 0, 9, 3, 2}},
    {"/b2set_zlib",       B2_F32, HDF5LIB_BLOSC2_ZLIB, 3, HDF5LIB_BLOSC2_TRUNC_PREC, -12,
     {B2_SETTINGS(8, -12), 0, 0, 0, 3, 4, 4}},
    {"/b2set_ndlz",       B2_I32, HDF5LIB_BLOSC2_NDLZ, 5, 0, 8, {B2_SETTINGS(0, 8), 0, 0, 0, 5, 0, 32}},
  };
  struct { unsigned int level, prefilters; int meta; } bad[] = {{10, 0, 0}, {5, 16, 0}, {5, 8, 256}, {5, 8, -129}};
  b2_data_t d;
  unsigned int vals[32], stored[32], flags;
  size_t nelmts, n_stored;
  hid_t plist = -1;
  int errors = 0;

  if (b2_data_init(&d) < 0) { errors++; goto cleanup; }
  for (size_t k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
    int t = cases[k].type;
    plist = H5Pcreate(H5P_DATASET_CREATE);
    nelmts = 32;
    int set = plist >= 0 &&
              hdf5lib_set_blosc2(plist, H5Z_FLAG_MANDATORY, cases[k].codec, cases[k].level,
                                 cases[k].prefilters, cases[k].meta) >= 0 &&
              H5Pget_filter_by_id(plist, H5Z_FILTER_BLOSC2, &flags, &nelmts, vals, 0, NULL, NULL) >= 0;
    if (plist >= 0) { H5Pclose(plist); plist = -1; }
    if (!set) {
      Rprintf("\n      -> %s: hdf5lib_set_blosc2() failed", cases[k].name);
      errors++;
    } else if (flags != H5Z_FLAG_MANDATORY || !b2_list_is(vals, nelmts, cases[k].want, 7)) {
      b2_print_list(cases[k].name, vals, nelmts);
      errors++;
    } else if (b2_store(fid, cases[k].name, d.type[t], 1, vals, nelmts, d.data[t], d.out,
                        stored, &n_stored, NULL, NULL) < 0) {
      Rprintf("\n      -> %s: could not be written and read", cases[k].name);
      errors++;
    } else if (!(cases[k].prefilters & HDF5LIB_BLOSC2_TRUNC_PREC) && cases[k].codec < HDF5LIB_BLOSC2_ZFP_ACC &&
               memcmp(d.data[t], d.out, B2_N * H5Tget_size(d.type[t])) != 0) {
      Rprintf("\n      -> %s: values changed", cases[k].name);
      errors++;
    }
  }

  /* A second call updates the filter in place: one Blosc2 filter, still ahead of Fletcher32 */
  unsigned int want_update[7] = {B2_SETTINGS(2, 0), 0, 0, 0, 7, 2, 1};
  nelmts = 32;
  if ((plist = H5Pcreate(H5P_DATASET_CREATE)) < 0 ||
      hdf5lib_set_blosc2(plist, H5Z_FLAG_MANDATORY, HDF5LIB_BLOSC2_ZSTD, 5, HDF5LIB_BLOSC2_SHUFFLE, 0) < 0 ||
      H5Pset_fletcher32(plist) < 0 ||
      hdf5lib_set_blosc2(plist, H5Z_FLAG_OPTIONAL, HDF5LIB_BLOSC2_LZ4, 7, HDF5LIB_BLOSC2_BITSHUFFLE, 0) < 0 ||
      H5Pget_nfilters(plist) != 2 ||
      H5Pget_filter2(plist, 0, &flags, &nelmts, vals, 0, NULL, NULL) != H5Z_FILTER_BLOSC2 ||
      flags != H5Z_FLAG_OPTIONAL || !b2_list_is(vals, nelmts, want_update, 7)) {
    Rprintf("\n      -> A second call did not update the Blosc2 filter in place");
    errors++;
  }
  if (plist >= 0) { H5Pclose(plist); plist = -1; }

  for (size_t k = 0; k < sizeof(bad) / sizeof(bad[0]); k++) {
    if ((plist = H5Pcreate(H5P_DATASET_CREATE)) < 0 ||
        hdf5lib_set_blosc2(plist, H5Z_FLAG_MANDATORY, HDF5LIB_BLOSC2_ZSTD, bad[k].level,
                           bad[k].prefilters, bad[k].meta) >= 0 ||
        H5Pget_nfilters(plist) != 0) {
      Rprintf("\n      -> Level %u, pre-filters %u, meta %d: not refused", bad[k].level, bad[k].prefilters, bad[k].meta);
      errors++;
    }
    if (plist >= 0) { H5Pclose(plist); plist = -1; }
  }

  /* Only a dataset creation property list takes a filter */
  if ((plist = H5Pcreate(H5P_FILE_CREATE)) < 0 ||
      hdf5lib_set_blosc2(plist, H5Z_FLAG_MANDATORY, HDF5LIB_BLOSC2_ZSTD, 5, 0, 0) >= 0) {
    Rprintf("\n      -> A file creation property list was not refused");
    errors++;
  }

cleanup:
  if (plist >= 0) H5Pclose(plist);
  b2_data_free(&d);
  return errors ? -1 : 0;
}

/* =========================================================================
 * MAIN R INVOCATION
 * ========================================================================= */
SEXP C_smoke_test(SEXP sexp_filename) {
  const char *filename = CHAR(STRING_ELT(sexp_filename, 0));
  char version_str[64];
  unsigned majnum, minnum, relnum;
  hid_t file_id = -1;
  SEXP sexp_result = R_NilValue;
  int total_errors = 0;

  H5Eset_auto(H5E_DEFAULT, NULL, NULL);

  /* Stands in for a real BitGroom plugin, which hdf5lib's shim must not replace */
  if (H5Zregister(&real_bitgroom_class) < 0) {
    Rf_error("C_smoke_test: could not register the BitGroom stand-in");
  }

  if (hdf5lib_register_all_filters() < 0) {
    Rf_error("C_smoke_test: hdf5lib_register_all_filters() failed");
  }

  if (H5get_libversion(&majnum, &minnum, &relnum) < 0) {
    Rf_error("C_smoke_test: H5get_libversion failed");
  }
  snprintf(version_str, sizeof(version_str), "%u.%u.%u", majnum, minnum, relnum);

  file_id = H5Fcreate(filename, H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
  if (file_id < 0) {
    Rf_error("C_smoke_test: H5Fcreate failed");
  }
  
  H5LTmake_dataset_string(file_id, "/version_str", version_str);

  FilterConfig filters[] = {
    {"zlibng_gzip", H5Z_FILTER_DEFLATE, 1, {9},                           TEST_ALL},
    {"szip_ec",     H5Z_FILTER_SZIP,    2, {H5_SZIP_EC_OPTION_MASK, 8},   TEST_NUMERIC_ONLY},
    {"szip_nn",     H5Z_FILTER_SZIP,    2, {H5_SZIP_NN_OPTION_MASK, 8},   TEST_NUMERIC_ONLY},
    {"bzip2",       H5Z_FILTER_BZIP2,   1, {9},                           TEST_ALL},
    {"lzf",         H5Z_FILTER_LZF,     0, {0},                           TEST_ALL},
    {"lz4",         H5Z_FILTER_LZ4,     2, {0, 0},                        TEST_ALL},
    {"lz4hc",       H5Z_FILTER_LZ4,     2, {0, 9},                        TEST_ALL},
    {"zstd",        H5Z_FILTER_ZSTD,    1, {3},                           TEST_ALL},
    {"snappy",      H5Z_FILTER_SNAPPY,  0, {0},                           TEST_ALL},
    {"bshuf_pure",  H5Z_FILTER_BSHUF,   2, {0, 0},                        TEST_ALL},
    {"bshuf_lz4",   H5Z_FILTER_BSHUF,   2, {0, 2},                        TEST_ALL},
    {"bshuf_zstd",  H5Z_FILTER_BSHUF,   3, {0, 3, 5},                     TEST_ALL},
    {"zfp_rate",    H5Z_FILTER_ZFP,     6, {1, 0, 16, 0, 0, 0},           TEST_NUMERIC_ONLY | TEST_NAN},
    {"zfp_prec",    H5Z_FILTER_ZFP,     6, {2, 0, 32, 0, 0, 0},           TEST_NUMERIC_ONLY | TEST_NAN},
    {"zfp_acc",     H5Z_FILTER_ZFP,     6, {3, 0, 3, 0, 0, 0},            TEST_FLOAT_ONLY | TEST_NAN},
    {"zfp_expert",  H5Z_FILTER_ZFP,     6, {4, 0, 1, 1024, 32, (unsigned int)-16}, TEST_NUMERIC_ONLY | TEST_NAN},
    {"zfp_rev",     H5Z_FILTER_ZFP,     6, {5, 0, 0, 0, 0, 0},            TEST_NUMERIC_ONLY | TEST_NAN},
    {"blosc_lz",    H5Z_FILTER_BLOSC,   7, {0, 0, 0, 0, 5, 1, 0},         TEST_ALL | TEST_THR},
    {"blosc_lz4",   H5Z_FILTER_BLOSC,   7, {0, 0, 0, 0, 5, 1, 1},         TEST_ALL | TEST_THR},
    {"blosc_lz4hc", H5Z_FILTER_BLOSC,   7, {0, 0, 0, 0, 5, 1, 2},         TEST_ALL | TEST_THR},
    {"blosc_snappy",H5Z_FILTER_BLOSC,   7, {0, 0, 0, 0, 5, 1, 3},         TEST_ALL | TEST_THR},
    {"blosc_zlib",  H5Z_FILTER_BLOSC,   7, {0, 0, 0, 0, 5, 1, 4},         TEST_ALL | TEST_THR},
    {"blosc_zstd",  H5Z_FILTER_BLOSC,   7, {0, 0, 0, 0, 5, 1, 5},         TEST_ALL | TEST_THR},
    {"blosc2_lz",   H5Z_FILTER_BLOSC2,  7, {B2_SETTINGS(1, 0), 0, 0, 0, 5, 1, 0},     TEST_ALL | TEST_THR},
    {"blosc2_lz4",  H5Z_FILTER_BLOSC2,  7, {B2_SETTINGS(1, 0), 0, 0, 0, 5, 1, 1},     TEST_ALL | TEST_THR},
    {"blosc2_lz4hc",H5Z_FILTER_BLOSC2,  7, {B2_SETTINGS(1, 0), 0, 0, 0, 5, 1, 2},     TEST_ALL | TEST_THR},
    {"blosc2_zlib", H5Z_FILTER_BLOSC2,  7, {B2_SETTINGS(1, 0), 0, 0, 0, 5, 1, 4},     TEST_ALL | TEST_THR},
    {"blosc2_zstd", H5Z_FILTER_BLOSC2,  7, {B2_SETTINGS(1, 0), 0, 0, 0, 5, 1, 5},     TEST_ALL | TEST_THR},
    {"blosc2_ndlz", H5Z_FILTER_BLOSC2,  7, {B2_SETTINGS(0, 4), 0, 0, 0, 5, 0, 32},    TEST_ND_ONLY | TEST_THR},
    {"b2_zfp_acc",  H5Z_FILTER_BLOSC2,  7, {B2_SETTINGS(0, 253), 0, 0, 0, 5, 0, 33},  TEST_FLOAT_ONLY | TEST_NAN | TEST_THR},
    {"b2_zfp_prec", H5Z_FILTER_BLOSC2,  7, {B2_SETTINGS(0, 16), 0, 0, 0, 5, 0, 34},   TEST_FLOAT_ONLY | TEST_NAN | TEST_THR},
    {"b2_zfp_rate", H5Z_FILTER_BLOSC2,  7, {B2_SETTINGS(0, 25), 0, 0, 0, 5, 0, 35},   TEST_FLOAT_ONLY | TEST_NAN | TEST_THR},
    /* Delta, then bitshuffle */
    {"blosc2_pipe", H5Z_FILTER_BLOSC2,  7, {B2_SETTINGS(6, 0), 0, 0, 0, 1, 2, 5},     TEST_NUMERIC_ONLY | TEST_THR}
  };
  
  int num_filters = sizeof(filters) / sizeof(FilterConfig);

  Rprintf("Running Strict HDF5 Plugin Smoke Tests...\n");
  for (int i = 0; i < num_filters; i++) {
    Rprintf("  Testing %-15s", filters[i].name);
    
    int err = 0;
    
    if (filters[i].test_mask & TEST_FLT) { if (test_float_data(file_id, &filters[i])               < 0) err++; }
    if (filters[i].test_mask & TEST_INT) { if (test_int_data(file_id, &filters[i])                 < 0) err++; }
    if (filters[i].test_mask & TEST_CMP) { if (test_compound_data(file_id, &filters[i])            < 0) err++; }
    if (filters[i].test_mask & TEST_ARR) { if (test_array_data(file_id, &filters[i])               < 0) err++; }
    if (filters[i].test_mask & TEST_ZRO) { if (test_highly_compressible_data(file_id, &filters[i]) < 0) err++; }
    if (filters[i].test_mask & TEST_RND) { if (test_incompressible_data(file_id, &filters[i])      < 0) err++; }
    if (filters[i].test_mask & TEST_UNC) { if (test_unchunked_data(file_id, &filters[i])           < 0) err++; }
    if (filters[i].test_mask & TEST_EMP) { if (test_empty_dataset(file_id, &filters[i])            < 0) err++; }
    if (filters[i].test_mask & TEST_2D8) { if (test_ndim2_data(file_id, &filters[i])               < 0) err++; }
    if (filters[i].test_mask & TEST_NAN) {
      if (test_nonfinite_data(file_id, &filters[i], H5Z_FLAG_OPTIONAL,  0) < 0) err++;
      if (test_nonfinite_data(file_id, &filters[i], H5Z_FLAG_OPTIONAL,  1) < 0) err++;
      if (test_nonfinite_data(file_id, &filters[i], H5Z_FLAG_MANDATORY, 0) < 0) err++;
    }
    if (filters[i].test_mask & TEST_THR) {
      /* CRAN allows two threads.  NDLZ only works on 2-D chunks. */
      int b2_codec = (filters[i].id == H5Z_FILTER_BLOSC2) ? (int)filters[i].cd_values[6] : -1;
      for (int nthreads = 1; nthreads <= 2; nthreads++) {
        if (b2_codec != 32 && test_threaded_data(file_id, &filters[i], nthreads, 1) < 0) err++;
        if (test_threaded_data(file_id, &filters[i], nthreads, 2) < 0) err++;
      }
    }
    
    if (err > 0) {
      Rprintf(" [FAILED]\n");
      total_errors++;
    } else {
      Rprintf(" [OK]\n");
    }
  }

  Rprintf("  Testing %-15s", "blosc_nthreads");
  if (test_blosc_nthreads_api() < 0) {
    Rprintf(" [FAILED]\n");
    total_errors++;
  } else {
    Rprintf(" [OK]\n");
  }

  Rprintf("  Testing %-15s", "blosc2_layout");
  int layout_errors = 0, layout_ranks[] = {1, 2, 17};  /* 17 > B2ND_MAX_DIM */
  for (int r = 0; r < 3; r++)
    if (test_blosc2_layout(file_id, layout_ranks[r]) < 0) layout_errors++;
  if (layout_errors > 0) {
    Rprintf(" [FAILED]\n");
    total_errors++;
  } else {
    Rprintf(" [OK]\n");
  }

  Rprintf("  Testing %-15s", "zfp_type_leak");
  if (test_zfp_type_leak(file_id) < 0) {
    Rprintf(" [FAILED]\n");
    total_errors++;
  } else {
    Rprintf(" [OK]\n");
  }

  Rprintf("  Testing %-15s", "zfp_unsupported");
  int unsupported_errors = 0;
  for (int which = 0; which < 4; which++) {
    if (test_zfp_unsupported(file_id, which, H5Z_FLAG_OPTIONAL)  < 0) unsupported_errors++;
    if (test_zfp_unsupported(file_id, which, H5Z_FLAG_MANDATORY) < 0) unsupported_errors++;
  }
  if (unsupported_errors > 0) {
    Rprintf(" [FAILED]\n");
    total_errors++;
  } else {
    Rprintf(" [OK]\n");
  }

  Rprintf("  Testing %-15s", "zfp_copied_dcpl");
  if (test_zfp_copied_dcpl(file_id, 0) < 0 || test_zfp_copied_dcpl(file_id, 1) < 0) {
    Rprintf(" [FAILED]\n");
    total_errors++;
  } else {
    Rprintf(" [OK]\n");
  }

  Rprintf("  Testing %-15s", "zfp_no_params");
  if (test_zfp_no_params() < 0) {
    Rprintf(" [FAILED]\n");
    total_errors++;
  } else {
    Rprintf(" [OK]\n");
  }

  Rprintf("  Testing %-15s", "blosc2_trunc");
  if (test_blosc2_trunc_types(file_id) < 0) {
    Rprintf(" [FAILED]\n");
    total_errors++;
  } else {
    Rprintf(" [OK]\n");
  }

  Rprintf("  Testing %-15s", "blosc2_copied");
  int copied_errors = 0;
  for (int rank = 1; rank <= 2; rank++)
    for (int zfp = 0; zfp <= 1; zfp++)
      if (test_blosc2_copied_dcpl(file_id, rank, zfp) < 0) copied_errors++;
  if (copied_errors > 0) {
    Rprintf(" [FAILED]\n");
    total_errors++;
  } else {
    Rprintf(" [OK]\n");
  }

  Rprintf("  Testing %-15s", "blosc2_zfp_type");
  if (test_blosc2_zfp_types(file_id) < 0) {
    Rprintf(" [FAILED]\n");
    total_errors++;
  } else {
    Rprintf(" [OK]\n");
  }

  Rprintf("  Testing %-15s", "shim_ids");
  if (test_shim_ids() < 0) {
    Rprintf(" [FAILED]\n");
    total_errors++;
  } else {
    Rprintf(" [OK]\n");
  }

  Rprintf("  Testing %-15s", "truncated");
  if (test_truncated_chunks(file_id) < 0) {
    Rprintf(" [FAILED]\n");
    total_errors++;
  } else {
    Rprintf(" [OK]\n");
  }

  Rprintf("  Testing %-15s", "blosc2_geometry");
  if (test_blosc2_geometry(file_id) < 0) {
    Rprintf(" [FAILED]\n");
    total_errors++;
  } else {
    Rprintf(" [OK]\n");
  }

  Rprintf("  Testing %-15s", "zfp_edges");
  if (test_zfp_edges(file_id) < 0) {
    Rprintf(" [FAILED]\n");
    total_errors++;
  } else {
    Rprintf(" [OK]\n");
  }

  Rprintf("  Testing %-15s", "bshuf_params");
  if (test_bshuf_params(file_id) < 0) {
    Rprintf(" [FAILED]\n");
    total_errors++;
  } else {
    Rprintf(" [OK]\n");
  }

  Rprintf("  Testing %-15s", "shim_defers");
  if (test_shim_defers(file_id) < 0) {
    Rprintf(" [FAILED]\n");
    total_errors++;
  } else {
    Rprintf(" [OK]\n");
  }

  Rprintf("  Testing %-15s", "snappy_layout");
  int snappy_errors = 0;
  if (test_snappy_written(file_id) < 0) snappy_errors++;
  if (test_snappy_raw(file_id) < 0) snappy_errors++;
  if (test_snappy_cuda_chunk(file_id) < 0) snappy_errors++;
  if (snappy_errors > 0) {
    Rprintf(" [FAILED]\n");
    total_errors++;
  } else {
    Rprintf(" [OK]\n");
  }

  Rprintf("  Testing %-15s", "blosc2_lists");
  if (test_blosc2_lists(file_id) < 0) {
    Rprintf(" [FAILED]\n");
    total_errors++;
  } else {
    Rprintf(" [OK]\n");
  }

  Rprintf("  Testing %-15s", "blosc2_forms");
  if (test_blosc2_input_forms(file_id) < 0) {
    Rprintf(" [FAILED]\n");
    total_errors++;
  } else {
    Rprintf(" [OK]\n");
  }

  Rprintf("  Testing %-15s", "blosc2_refused");
  if (test_blosc2_refused(file_id) < 0) {
    Rprintf(" [FAILED]\n");
    total_errors++;
  } else {
    Rprintf(" [OK]\n");
  }

  Rprintf("  Testing %-15s", "blosc2_setter");
  if (test_blosc2_setter(file_id) < 0) {
    Rprintf(" [FAILED]\n");
    total_errors++;
  } else {
    Rprintf(" [OK]\n");
  }

  H5Fclose(file_id);
  hdf5lib_destroy_all_filters();

  Rprintf("  Testing %-15s", "unregistered");
  if (test_filters_unregistered() < 0) {
    Rprintf(" [FAILED]\n");
    total_errors++;
  } else {
    Rprintf(" [OK]\n");
  }

  if (total_errors > 0) {
    Rf_error("HDF5 Plugin Smoke Tests Failed! (%d filters broken). Check compilation and linking.", total_errors);
  }

  sexp_result = PROTECT(Rf_allocVector(STRSXP, 1));
  SET_STRING_ELT(sexp_result, 0, Rf_mkChar(version_str));
  UNPROTECT(1);
  return sexp_result;
}
