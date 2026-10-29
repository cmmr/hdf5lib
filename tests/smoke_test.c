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
#define H5Z_FILTER_BITSHAVE  32023
#define H5Z_FILTER_BITROUND  32024
#define H5Z_FILTER_BLOSC2    32026

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

/* --- Filter Configuration Architecture --- */
typedef struct {
  const char* name;
  H5Z_filter_t id;
  size_t       nelmts;
  unsigned int cd_values[12];
  int          test_mask;
} FilterConfig;

/* Declare external helpers provided by the bundled ZFP plugin */
extern herr_t H5Pset_zfp_rate(hid_t plist, double rate);
extern herr_t H5Pset_zfp_precision(hid_t plist, unsigned int prec);
extern herr_t H5Pset_zfp_accuracy(hid_t plist, double acc);
extern herr_t H5Pset_zfp_expert(hid_t plist, unsigned int minbits, unsigned int maxbits, unsigned int maxprec, int minexp);
extern herr_t H5Pset_zfp_reversible(hid_t plist);

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

  CHECK(H5Dread(dset, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, H5P_DEFAULT, data_out), "H5Dread");

  for(int i=0; i<FLT_SIZE; i++) {
    /* Tolerance of 1.0f easily supports lossy rates (e.g. ZFP rate=8) */
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
  /* The other chunks went through lossy ZFP.  zfp_expert keeps only 4 bits a
     value, so for it just check that they decoded to finite values in range. */
  int coarse = (cfg->id == H5Z_FILTER_ZFP && cfg->cd_values[0] == 4);
  for (hsize_t i = 0; i < dims[0]; i++) {
    if (i / NF_CHUNK == NF_BAD) continue;
    double a, b;
    if (use_float) { float fa, fb; memcpy(&fa, data + i * tsize, tsize); memcpy(&fb, data_out + i * tsize, tsize); a = fa; b = fb; }
    else           { memcpy(&a, data + i * tsize, tsize); memcpy(&b, data_out + i * tsize, tsize); }
    if (coarse ? !(fabs(b) <= 200.0) : !(fabs(a - b) <= 1.0 + 0.1 * fabs(a))) {
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
   cannot hold, must store exactly the seven values it writes, with our meta
   value in the high bits of slot 5; ranks 2..16 add rank, shape and meta.
   Trunc-prec makes the meta value observable: keeping 8 bits, the low 15
   mantissa bits of every float32 must read back as zero. */
static int test_blosc2_layout(hid_t fid, int rank) {
  hsize_t dims[17], chunkdims[17], n = 4096;
  float *data = malloc(n * sizeof(float)), *data_out = malloc(n * sizeof(float));
  unsigned int cd[8] = {0, 0, 0, 0, 5, 8 | 2, 1, 8};  /* trunc-prec(8) + bitshuffle, lz4 */
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
  CHECK(H5Pset_filter(plist, H5Z_FILTER_BLOSC2, H5Z_FLAG_MANDATORY, 8, cd), "H5Pset_filter");

  char dset_name[64];
  snprintf(dset_name, sizeof(dset_name), "/blosc2_layout_%dd", rank);
  CHECK(dset = H5Dcreate2(fid, dset_name, H5T_NATIVE_FLOAT, sid, H5P_DEFAULT, plist, H5P_DEFAULT), "H5Dcreate2");
  CHECK(H5Dwrite(dset, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, H5P_DEFAULT, data), "H5Dwrite");
  if (H5Dclose(dset) < 0) { dset = -1; Rprintf("\n      -> H5Dclose failed"); goto cleanup; }

  CHECK(dset = H5Dopen2(fid, dset_name, H5P_DEFAULT), "H5Dopen2");
  CHECK(dcpl = H5Dget_create_plist(dset), "H5Dget_create_plist");
  CHECK(H5Pget_filter_by_id(dcpl, H5Z_FILTER_BLOSC2, &flags, &nelmts, vals, 0, NULL, NULL), "H5Pget_filter_by_id");

  int layout_ok = (rank == 2)
    ? (nelmts == 11 && vals[5] == 10 && vals[7] == 2 && vals[8] == 32 && vals[9] == 32 && vals[10] == 8)
    : (nelmts == 7 && vals[5] == (10 | 8 << 8));
  if (!layout_ok || vals[3] != (unsigned int)(n / 4 * sizeof(float))) {
    Rprintf("\n      -> Rank %d stored %zu cd_values (slot 5 = %u)", rank, nelmts, vals[5]);
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
  unsigned int cd[8] = {0, 0, 0, 0, 5, 8 | 2, 1, (unsigned int)prec};
  unsigned int vals[32], flags;
  size_t nelmts = 32;
  hid_t sid = -1, plist = -1, dset = -1, dcpl = -1;
  int ret = -1;

  CHECK(sid = H5Screate_simple(rank, dims, NULL), "H5Screate_simple");
  CHECK(plist = H5Pcreate(H5P_DATASET_CREATE), "H5Pcreate");
  CHECK(H5Pset_chunk(plist, rank, chunkdims), "H5Pset_chunk");
  CHECK(H5Pset_filter(plist, H5Z_FILTER_BLOSC2, H5Z_FLAG_MANDATORY, 8, cd), "H5Pset_filter");
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
  ret = (int)(vals[5] & 0xFF);

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

  /* Integers, in 1-D and 2-D chunks (which keep the precision in different slots) */
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
   geometry the list holds the rank and shape before it, and the rank used to
   be taken for it: a 2-D copy kept 2 bits instead of 8, or turned ZFP's
   accuracy of 10^-3 into 10^2.  1-D chunks keep it in slot 5 instead. */
static int test_blosc2_copied_dcpl(hid_t fid, int rank, int zfp) {
  hsize_t dims[2] = {4096, 1}, chunkdims[2] = {1024, 1};
  if (rank == 2) { dims[0] = dims[1] = 64; chunkdims[0] = chunkdims[1] = 32; }
  size_t n = 4096;
  unsigned int meta = zfp ? 253 : 8;  /* accuracy 10^-3, or keep 8 bits */
  unsigned int cd[8] = {0, 0, 0, 0, 5, zfp ? 0 : 8 | 2, zfp ? 33 : 1, meta};
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
  CHECK(H5Pset_filter(plist, H5Z_FILTER_BLOSC2, H5Z_FLAG_MANDATORY, 8, cd), "H5Pset_filter");
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

  unsigned int copied = (rank == 1) ? (vals[5] >> 8) & 0xFF : vals[nelmts - 1];
  if (copied != meta) {
    Rprintf("\n      -> %dD %s copy recorded meta value %u, not %u", rank, zfp ? "ZFP" : "trunc-prec", copied, meta);
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

/* 20. Tear-Down Unregisters Every Filter */
static int test_filters_unregistered(void) {
  H5Z_filter_t ids[] = {H5Z_FILTER_BLOSC, H5Z_FILTER_BLOSC2, H5Z_FILTER_BSHUF, H5Z_FILTER_BZIP2,
                        H5Z_FILTER_LZ4, H5Z_FILTER_LZF, H5Z_FILTER_SNAPPY, H5Z_FILTER_ZFP,
                        H5Z_FILTER_ZSTD, H5Z_FILTER_BITGROOM, H5Z_FILTER_BITSHAVE, H5Z_FILTER_BITROUND};
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
  H5PLset_loading_state(state);
  return ret;
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
    {"zfp_rate",    H5Z_FILTER_ZFP,     6, {1, 0, 8, 0, 0, 0},            TEST_NUMERIC_ONLY | TEST_NAN},
    {"zfp_prec",    H5Z_FILTER_ZFP,     6, {2, 0, 16, 0, 0, 0},           TEST_NUMERIC_ONLY | TEST_NAN},
    {"zfp_acc",     H5Z_FILTER_ZFP,     6, {3, 0, 3, 0, 0, 0},            TEST_FLOAT_ONLY | TEST_NAN},
    {"zfp_expert",  H5Z_FILTER_ZFP,     6, {4, 0, 1, 16, 16, 0},          TEST_NUMERIC_ONLY | TEST_NAN},
    {"zfp_rev",     H5Z_FILTER_ZFP,     6, {5, 0, 0, 0, 0, 0},            TEST_NUMERIC_ONLY | TEST_NAN},
    {"blosc_lz",    H5Z_FILTER_BLOSC,   7, {0, 0, 0, 0, 5, 1, 0},         TEST_ALL | TEST_THR},
    {"blosc_lz4",   H5Z_FILTER_BLOSC,   7, {0, 0, 0, 0, 5, 1, 1},         TEST_ALL | TEST_THR},
    {"blosc_lz4hc", H5Z_FILTER_BLOSC,   7, {0, 0, 0, 0, 5, 1, 2},         TEST_ALL | TEST_THR},
    {"blosc_snappy",H5Z_FILTER_BLOSC,   7, {0, 0, 0, 0, 5, 1, 3},         TEST_ALL | TEST_THR},
    {"blosc_zlib",  H5Z_FILTER_BLOSC,   7, {0, 0, 0, 0, 5, 1, 4},         TEST_ALL | TEST_THR},
    {"blosc_zstd",  H5Z_FILTER_BLOSC,   7, {0, 0, 0, 0, 5, 1, 5},         TEST_ALL | TEST_THR},
    {"blosc2_lz",   H5Z_FILTER_BLOSC2,  7, {0, 0, 0, 0, 5, 1, 0},         TEST_ALL | TEST_THR},
    {"blosc2_lz4",  H5Z_FILTER_BLOSC2,  7, {0, 0, 0, 0, 5, 1, 1},         TEST_ALL | TEST_THR},
    {"blosc2_lz4hc",H5Z_FILTER_BLOSC2,  7, {0, 0, 0, 0, 5, 1, 2},         TEST_ALL | TEST_THR},
    {"blosc2_zlib", H5Z_FILTER_BLOSC2,  7, {0, 0, 0, 0, 5, 1, 4},         TEST_ALL | TEST_THR},
    {"blosc2_zstd", H5Z_FILTER_BLOSC2,  7, {0, 0, 0, 0, 5, 1, 5},         TEST_ALL | TEST_THR},
    {"blosc2_ndlz", H5Z_FILTER_BLOSC2,  8, {0, 0, 0, 0, 5, 0, 32, 4},     TEST_ND_ONLY | TEST_THR},
    {"b2_zfp_acc",  H5Z_FILTER_BLOSC2,  8, {0, 0, 0, 0, 5, 0, 33, 253},   TEST_FLOAT_ONLY | TEST_NAN | TEST_THR},
    {"b2_zfp_prec", H5Z_FILTER_BLOSC2,  8, {0, 0, 0, 0, 5, 0, 34, 16},    TEST_FLOAT_ONLY | TEST_NAN | TEST_THR},
    {"b2_zfp_rate", H5Z_FILTER_BLOSC2,  8, {0, 0, 0, 0, 5, 0, 35,  25},   TEST_FLOAT_ONLY | TEST_NAN | TEST_THR},
    {"blosc2_pipe", H5Z_FILTER_BLOSC2,  7, {0, 0, 0, 0, 1, 6, 5},         TEST_NUMERIC_ONLY | TEST_THR}
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
