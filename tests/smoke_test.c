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

   ZFP's integer transforms shift and overflow signed values in ways UBSan
   reports (an upstream issue), so this sticks to cases that stay clear of it:
   the finite values follow the float test's pattern, every 4-value block an
   increasing ramp, and neither reversible mode (which keeps NaN and Inf exactly
   and is exempt, but via overflow) nor 4-bit expert mode is run here.  The
   guard runs before any mode-specific code, so the other modes cover it. */
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

  if (!data || !data_out) goto cleanup;

  for (hsize_t i = 0; i < dims[0]; i++) {
    double v = (double)(i % 100);
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

  if (flags == H5Z_FLAG_MANDATORY) {
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
    int expect_skipped = (offset[0] / NF_CHUNK == NF_BAD);
    if (skipped != expect_skipped) {
      Rprintf("\n      -> Chunk at %llu was %sfiltered (%s)", (unsigned long long)offset[0],
              skipped ? "not " : "", use_float ? "f32" : "f64");
      goto cleanup;
    }
  }

  /* The non-finite chunk must be bit-exact */
  if (memcmp(data + bad * tsize, data_out + bad * tsize, NF_CHUNK * tsize) != 0) {
    Rprintf("\n      -> Non-finite chunk did not round-trip exactly (%s)", use_float ? "f32" : "f64");
    goto cleanup;
  }
  /* The other chunks went through lossy ZFP */
  for (hsize_t i = 0; i < dims[0]; i++) {
    if (i / NF_CHUNK == NF_BAD) continue;
    double a, b;
    if (use_float) { float fa, fb; memcpy(&fa, data + i * tsize, tsize); memcpy(&fb, data_out + i * tsize, tsize); a = fa; b = fb; }
    else           { memcpy(&a, data + i * tsize, tsize); memcpy(&b, data_out + i * tsize, tsize); }
    if (!(fabs(a - b) <= 1.0)) {
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
   does.  Lossy codecs are held to a tolerance; the rest must be exact.  ZFP
   gets ramps that restart on block boundaries (see the non-finite test). */
#define THR_CHUNK_1D 262144   /* 2 MB of doubles */
#define THR_CHUNK_2D 512      /* 512 x 512 doubles, also 2 MB */

static double thr_value(hsize_t row, hsize_t col, int zfp) {
  if (zfp) return (double)(col % 100);
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
    if (rank == 1) data[i] = thr_value(0, i % THR_CHUNK_1D, lossy);
    else           data[i] = thr_value((i / dims[1]) % THR_CHUNK_2D, (i % dims[1]) % THR_CHUNK_2D, lossy);
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
    {"zfp_expert",  H5Z_FILTER_ZFP,     6, {4, 0, 1, 16, 16, 0},          TEST_NUMERIC_ONLY},
    {"zfp_rev",     H5Z_FILTER_ZFP,     6, {5, 0, 0, 0, 0, 0},            TEST_NUMERIC_ONLY},
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
      /* CRAN allows two threads.  NDLZ only works on 2-D chunks; ZFP's 2-D
         decoder trips UBSan (see the non-finite test), and ZFP takes the same
         B2ND path in 1-D anyway. */
      int b2_codec = (filters[i].id == H5Z_FILTER_BLOSC2) ? (int)filters[i].cd_values[6] : -1;
      int zfp = (b2_codec >= 33 && b2_codec <= 35);
      for (int nthreads = 1; nthreads <= 2; nthreads++) {
        if (b2_codec != 32 && test_threaded_data(file_id, &filters[i], nthreads, 1) < 0) err++;
        if (!zfp && test_threaded_data(file_id, &filters[i], nthreads, 2) < 0) err++;
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

  H5Fclose(file_id);
  hdf5lib_destroy_all_filters();

  if (total_errors > 0) {
    Rf_error("HDF5 Plugin Smoke Tests Failed! (%d filters broken). Check compilation and linking.", total_errors);
  }

  sexp_result = PROTECT(Rf_allocVector(STRSXP, 1));
  SET_STRING_ELT(sexp_result, 0, Rf_mkChar(version_str));
  UNPROTECT(1);
  return sexp_result;
}
