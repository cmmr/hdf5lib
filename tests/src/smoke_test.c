#include <R.h>
#include <Rinternals.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <hdf5.h>
#include <hdf5_hl.h>
#include "hdf5lib.h"

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

#define TEST_ALL          (TEST_FLT | TEST_INT | TEST_CMP | TEST_ARR | TEST_ZRO | TEST_RND | TEST_UNC | TEST_EMP)
/* Filters like standalone ZFP and SZIP officially support atomic numerical arrays (Float & Int) */
#define TEST_NUMERIC_ONLY (TEST_FLT | TEST_INT | TEST_ZRO | TEST_UNC | TEST_EMP)
/* Blosc2's internal ZFP codec hard-fails on integers; strictly limit it to float paths */
#define TEST_FLOAT_ONLY   (TEST_FLT | TEST_ZRO | TEST_EMP)

/* --- Verbose Error Macro --- */
#define CHECK(expr, step_msg) do { \
  if ((expr) < 0) { \
    Rprintf("\n      -> %s failed", step_msg); \
    goto cleanup; \
  } \
} while(0)

/* --- Filter Configuration Architecture --- */
typedef struct {
  const char* name;
  H5Z_filter_t id;
  size_t       nelmts;
  unsigned int cd_values[12]; /* Increased from 7 to 12 to support Blosc2 pipelines */
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
    /* Route standalone ZFP through the safe C helpers to avoid raw double memory packing issues */
    unsigned int mode = cfg->cd_values[0];
    if (mode == 1) return H5Pset_zfp_rate(plist, (double)cfg->cd_values[2]);
    if (mode == 2) return H5Pset_zfp_precision(plist, cfg->cd_values[2]);
    if (mode == 3) {
      /* Convert integer representation (e.g. 3) to an absolute tolerance (0.001) */
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

/* A function to read the raw chunk from disk and extract the Blosc compcode */
static void verify_blosc_compcode(hid_t dset) {
    hsize_t offset[1] = {0}; /* We only need the first chunk */
    hsize_t chunk_bytes;
    uint32_t filter_mask = 0;

    /* 1. Get the physical size of the chunk on disk */
    if (H5Dget_chunk_info(dset, H5P_DEFAULT, 0, offset, (unsigned *)&filter_mask, NULL, &chunk_bytes) < 0) {
        Rprintf("\n      -> Codec Inspector: Failed to get chunk info.");
        return;
    }

    unsigned char *raw_chunk = malloc((size_t)chunk_bytes);
    if (!raw_chunk) return;

    size_t chunk_buf_size = (size_t)chunk_bytes;

    /* 2. Read the raw compressed bytes directly, bypassing decompression */
    if (H5Dread_chunk(dset, H5P_DEFAULT, offset, &filter_mask, raw_chunk, &chunk_buf_size) < 0) {
        Rprintf("\n      -> Codec Inspector: Failed to read raw chunk.");
        free(raw_chunk);
        return;
    }

    /* 3. Identify the Chunk Architecture */
    if (raw_chunk[2] == 'b' && raw_chunk[3] == '2' && raw_chunk[4] == 'f') {
        Rprintf("\n      -> Disk Inspection: Successfully wrote Blosc2 Frame (b2frame)");
    } else {
        /* Blosc1 stores the compformat in the top 3 bits of byte 2 */
        unsigned char flags = raw_chunk[2];
        unsigned char compformat = (flags >> 5) & 7;
        
        if (compformat == 0) {
            Rprintf("\n      -> Disk Inspection: Blosc1 Stamped Codec 0 (blosclz)");
        } else {
            Rprintf("\n      -> Disk Inspection: Blosc1 Stamped Codec %d", compformat);
        }
    }

    free(raw_chunk);
}

/* =========================================================================
 * DATATYPE TEST RUNNERS
 * ========================================================================= */

/* 1. Float / Tiny Chunks Test */
static int test_float_data(hid_t fid, const FilterConfig* cfg) {
  #define FLT_SIZE 1024
  float data[FLT_SIZE], data_out[FLT_SIZE];
  hsize_t dims[1] = {FLT_SIZE}, chunkdims[1] = {128};
  hid_t sid = -1, plist = -1, dset = -1;
  int ret = -1;

  for(int i=0; i<FLT_SIZE; i++) { 
    /* The Proof Array: Repeats every 64 elements (LZ loves this). 
       Smooth slope (ZFP loves this). */
    data[i] = (float)(i % 64) * 0.5f; 
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

  /* This forces the compression pipeline to execute and traps any crashes (missing plugins) */
  CHECK(H5Dflush(dset), "H5Dflush (Compression Pipeline Crashed!)"); 

  /* Inspect the raw disk bytes to see what Blosc ACTUALLY did */
  if (cfg->id == H5Z_FILTER_BLOSC2 || cfg->id == H5Z_FILTER_BLOSC) {
      verify_blosc_compcode(dset);
  }

  hsize_t allocated_size = H5Dget_storage_size(dset);
  hsize_t uncompressed_size = FLT_SIZE * sizeof(float);

  /* Bypass size check for purely structural filters (e.g., pure bitshuffle without LZ4/Zstd) */
  int skip_size_check = 0;
  if (cfg->id == H5Z_FILTER_BSHUF && cfg->cd_values[1] == 0) {
      skip_size_check = 1;
  }

  /* A true compressor will crush this repeating smooth array easily. */
  if (!skip_size_check && allocated_size >= (uncompressed_size * 0.9)) {
      Rprintf("\n      -> Silent Failure: Float data was not compressed (Allocated: %llu, Raw: %llu)", 
              (unsigned long long)allocated_size, (unsigned long long)uncompressed_size);
      goto cleanup;
  }

  CHECK(H5Dread(dset, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, H5P_DEFAULT, data_out), "H5Dread");

  for(int i=0; i<FLT_SIZE; i++) {
    if(data[i] - data_out[i] > 0.1 || data_out[i] - data[i] > 0.1) {
      Rprintf("\n      -> Float mismatch at index %d", i);
      goto cleanup;
    }
  }
  ret = 0;

cleanup:
  if (dset >= 0) H5Dclose(dset);
  if (plist >= 0) H5Pclose(plist);
  if (sid >= 0) H5Sclose(sid);
  return ret;
}

/* 2. Integer Data Test */
static int test_int_data(hid_t fid, const FilterConfig* cfg) {
  #define INT_SIZE 1024
  int data[INT_SIZE], data_out[INT_SIZE];
  hsize_t dims[1] = {INT_SIZE}, chunkdims[1] = {128};
  hid_t sid = -1, plist = -1, dset = -1;
  int ret = -1;

  for(int i=0; i<INT_SIZE; i++) { 
    /* THE OBLIVION SHIFT TRAP: 
       Alternates integers representing large floats and small floats.
       The "small float" bits will be shifted into oblivion by Blosc2 ZFP float-alignment. */
    if (i % 2 == 0) {
        data[i] = 1275068415; /* "Large" Float (Exp 151) */
    } else {
        data[i] = 822083583;  /* "Small" Float (Exp 96) */
    }
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

  /* This forces the compression pipeline to execute and traps any crashes (missing plugins) */
  CHECK(H5Dflush(dset), "H5Dflush (Compression Pipeline Crashed!)"); 

  /* Inspect the raw disk bytes to see what Blosc ACTUALLY did */
  if (cfg->id == H5Z_FILTER_BLOSC2 || cfg->id == H5Z_FILTER_BLOSC) {
      verify_blosc_compcode(dset);
  }

  hsize_t allocated_size = H5Dget_storage_size(dset);
  hsize_t uncompressed_size = INT_SIZE * sizeof(int);

  /* Bypass size check for purely structural filters (e.g., pure bitshuffle without LZ4/Zstd) */
  int skip_size_check = 0;
  if (cfg->id == H5Z_FILTER_BSHUF && cfg->cd_values[1] == 0) {
      skip_size_check = 1;
  }

  if (!skip_size_check && allocated_size >= (uncompressed_size * 0.9)) {
      Rprintf("\n      -> Silent Failure: Integer data was not compressed (Allocated: %llu, Raw: %llu)", 
              (unsigned long long)allocated_size, (unsigned long long)uncompressed_size);
      goto cleanup;
  }

  CHECK(H5Dread(dset, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, data_out), "H5Dread");

  /* Tolerance increased to 40,000. Standalone ZFP prec=16 on 2.1-Billion 
     drops ~15 bits of mantissa. 2^15 = 32768 expected error margin. */
  for(int i=0; i<INT_SIZE; i++) {
    if(abs(data[i] - data_out[i]) > 40000) {
      Rprintf("\n      -> Int mismatch at index %d (in: %d, out: %d)", i, data[i], data_out[i]);
      goto cleanup;
    }
  }
  ret = 0;

cleanup:
  if (dset >= 0) H5Dclose(dset);
  if (plist >= 0) H5Pclose(plist);
  if (sid >= 0) H5Sclose(sid);
  return ret;
}

/* 3. Compound Data Test (> 255 bytes to test Blosc fallback) */
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
  if (dset >= 0) H5Dclose(dset);
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
  if (dset >= 0) H5Dclose(dset);
  if (dtype >= 0) H5Tclose(dtype);
  if (plist >= 0) H5Pclose(plist);
  if (sid >= 0) H5Sclose(sid);
  return ret;
}

/* 5. Highly Compressible Data Test (Tests Memory Resizing during Decompression) */
static int test_highly_compressible_data(hid_t fid, const FilterConfig* cfg) {
  #define HC_SIZE 100000
  float *data = NULL, *data_out = NULL;
  hsize_t dims[1] = {HC_SIZE}, chunkdims[1] = {10000};
  hid_t sid = -1, plist = -1, dset = -1;
  int ret = -1;

  data = malloc(HC_SIZE * sizeof(float));
  data_out = malloc(HC_SIZE * sizeof(float));
  if (!data || !data_out) goto cleanup;
  
  /* Constant values compress massively, forcing decompression buffer loops to work */
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
  if (dset >= 0) H5Dclose(dset);
  if (plist >= 0) H5Pclose(plist);
  if (sid >= 0) H5Sclose(sid);
  return ret;
}

/* 6. Incompressible Data Test (Tests Plugin Fallback paths natively) */
static int test_incompressible_data(hid_t fid, const FilterConfig* cfg) {
  #define RND_SIZE 10000
  unsigned char *data = NULL, *data_out = NULL;
  hsize_t dims[1] = {RND_SIZE}, chunkdims[1] = {1000};
  hid_t sid = -1, plist = -1, dset = -1;
  int ret = -1;

  data = malloc(RND_SIZE);
  data_out = malloc(RND_SIZE);
  if (!data || !data_out) goto cleanup;
  
  /* Pseudo-random generation ensures algorithms cannot compress it */
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
  if (dset >= 0) H5Dclose(dset);
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
  
  /* Explicitly DO NOT chunk. HDF5 filters strictly require a chunked layout.
     Applying a filter over a contiguous layout will result in an HDF5 failure.
     This function acts as a control to ensure basic I/O succeeds. */

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
  if (dset >= 0) H5Dclose(dset);
  if (plist >= 0) H5Pclose(plist);
  if (sid >= 0) H5Sclose(sid);
  return ret;
}

/* 8. Empty Dataset Logic Path (Tests `if (nbytes == 0)` short-circuits) */
static int test_empty_dataset(hid_t fid, const FilterConfig* cfg) {
  hsize_t dims[1] = {0}; /* 0 Elements Extent */
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
  
  /* Trigger the I/O pipeline for an empty layout */
  float dummy = 0.0f;
  CHECK(H5Dwrite(dset, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, H5P_DEFAULT, &dummy), "H5Dwrite");
  CHECK(H5Dread(dset, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, H5P_DEFAULT, &dummy), "H5Dread");

  ret = 0;

cleanup:
  if (dset >= 0) H5Dclose(dset);
  if (plist >= 0) H5Pclose(plist);
  if (sid >= 0) H5Sclose(sid);
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

  /* Silence HDF5 internal error printing so the R console stays clean */
  H5Eset_auto(H5E_DEFAULT, NULL, NULL);

  /* Register Custom Plugins */
  if (hdf5lib_register_all_filters() < 0) {
    Rf_error("C_smoke_test: hdf5lib_register_all_filters() failed");
  }

  /* Get HDF5 Version */
  if (H5get_libversion(&majnum, &minnum, &relnum) < 0) {
    Rf_error("C_smoke_test: H5get_libversion failed");
  }
  snprintf(version_str, sizeof(version_str), "%u.%u.%u", majnum, minnum, relnum);

  /* Open File */
  file_id = H5Fcreate(filename, H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
  if (file_id < 0) {
    Rf_error("C_smoke_test: H5Fcreate failed");
  }
  
  H5LTmake_dataset_string(file_id, "/version_str", version_str);

  /* Define Filters to test */
  FilterConfig filters[] = {
    /* Standard Library Chaining */
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
    
    /* Standalone ZFP supports int/float logic */
    {"zfp_rate",    H5Z_FILTER_ZFP,     6, {1, 0, 8, 0, 0, 0},            TEST_NUMERIC_ONLY},
    {"zfp_prec",    H5Z_FILTER_ZFP,     6, {2, 0, 16, 0, 0, 0},           TEST_NUMERIC_ONLY},
    {"zfp_acc",     H5Z_FILTER_ZFP,     6, {3, 0, 3, 0, 0, 0},            TEST_FLOAT_ONLY}, /* Fractional acc invalid for ints */
    {"zfp_expert",  H5Z_FILTER_ZFP,     6, {4, 0, 1, 16, 16, 0},          TEST_NUMERIC_ONLY},
    {"zfp_rev",     H5Z_FILTER_ZFP,     6, {5, 0, 0, 0, 0, 0},            TEST_NUMERIC_ONLY},
    
    /* Legacy Blosc1 Architecture */
    {"blosc_lz",    H5Z_FILTER_BLOSC,   7, {0, 0, 0, 0, 5, 1, 0},         TEST_ALL},
    {"blosc_lz4",   H5Z_FILTER_BLOSC,   7, {0, 0, 0, 0, 5, 1, 1},         TEST_ALL},
    {"blosc_lz4hc", H5Z_FILTER_BLOSC,   7, {0, 0, 0, 0, 5, 1, 2},         TEST_ALL},
    {"blosc_snappy",H5Z_FILTER_BLOSC,   7, {0, 0, 0, 0, 5, 1, 3},         TEST_ALL},
    {"blosc_zlib",  H5Z_FILTER_BLOSC,   7, {0, 0, 0, 0, 5, 1, 4},         TEST_ALL},
    {"blosc_zstd",  H5Z_FILTER_BLOSC,   7, {0, 0, 0, 0, 5, 1, 5},         TEST_ALL},

    /* Modern Blosc2 Architecture */
    {"blosc2_lz",   H5Z_FILTER_BLOSC2,  7, {0, 0, 0, 0, 5, 1, 0},         TEST_ALL},
    {"blosc2_lz4",  H5Z_FILTER_BLOSC2,  7, {0, 0, 0, 0, 5, 1, 1},         TEST_ALL},
    {"blosc2_lz4hc",H5Z_FILTER_BLOSC2,  7, {0, 0, 0, 0, 5, 1, 2},         TEST_ALL},
    {"blosc2_snappy",H5Z_FILTER_BLOSC2, 7, {0, 0, 0, 0, 5, 1, 3},         TEST_ALL},
    {"blosc2_zlib", H5Z_FILTER_BLOSC2,  7, {0, 0, 0, 0, 5, 1, 4},         TEST_ALL},
    {"blosc2_zstd", H5Z_FILTER_BLOSC2,  7, {0, 0, 0, 0, 5, 1, 5},         TEST_ALL},
    {"blosc2_ndlz", H5Z_FILTER_BLOSC2,  7, {0, 0, 0, 0, 5, 1, 11},        TEST_ARR},
    
    /* Blosc2 + ZFP Codecs strict limits (TEST_FLOAT_ONLY) */
    {"b2_zfp_acc",  H5Z_FILTER_BLOSC2,  8, {0, 0, 0, 0, 5, 0, 33,  3},    TEST_FLOAT_ONLY}, 
    {"b2_zfp_prec", H5Z_FILTER_BLOSC2,  8, {0, 0, 0, 0, 5, 0, 34, 16},    TEST_FLOAT_ONLY}, 
    {"b2_zfp_rate", H5Z_FILTER_BLOSC2,  8, {0, 0, 0, 0, 5, 0, 35,  8},    TEST_FLOAT_ONLY}, 
    
    /* Blosc2 Programmable Filter Pipeline (Delta -> Bitshuffle -> Zstd) */
    {"blosc2_pipe", H5Z_FILTER_BLOSC2, 12, {0, 0, 0, 0, 5, 2, 5, 2, 3, 2, 0, 0}, TEST_ALL}
  };
  
  int num_filters = sizeof(filters) / sizeof(FilterConfig);

  Rprintf("Running Strict HDF5 Plugin Smoke Tests...\n");
  for (int i = 0; i < num_filters; i++) {
    Rprintf("  Testing %-15s", filters[i].name);
    
    int err = 0;
    
    /* Execute expected combinations */
    if (filters[i].test_mask & TEST_FLT) { if (test_float_data(file_id, &filters[i])               < 0) err++; }
    if (filters[i].test_mask & TEST_INT) { if (test_int_data(file_id, &filters[i])                 < 0) err++; }
    if (filters[i].test_mask & TEST_CMP) { if (test_compound_data(file_id, &filters[i])            < 0) err++; }
    if (filters[i].test_mask & TEST_ARR) { if (test_array_data(file_id, &filters[i])               < 0) err++; }
    if (filters[i].test_mask & TEST_ZRO) { if (test_highly_compressible_data(file_id, &filters[i]) < 0) err++; }
    if (filters[i].test_mask & TEST_RND) { if (test_incompressible_data(file_id, &filters[i])      < 0) err++; }
    if (filters[i].test_mask & TEST_UNC) { if (test_unchunked_data(file_id, &filters[i])           < 0) err++; }
    if (filters[i].test_mask & TEST_EMP) { if (test_empty_dataset(file_id, &filters[i])            < 0) err++; }
    
    if (err > 0) {
      Rprintf(" [FAILED]\n");
      total_errors++;
    } else {
      Rprintf(" [OK]\n");
    }
  }

  H5Fclose(file_id);

  /* Destroy global filter state to prevent Valgrind TLS memory leaks */
  hdf5lib_destroy_all_filters();

  /* Enforce Strict Failure */
  if (total_errors > 0) {
    Rf_error("HDF5 Plugin Smoke Tests Failed! (%d filters broken). Check compilation and linking.", total_errors);
  }

  /* Return Version String to R */
  sexp_result = PROTECT(Rf_allocVector(STRSXP, 1));
  SET_STRING_ELT(sexp_result, 0, Rf_mkChar(version_str));
  UNPROTECT(1);
  return sexp_result;
}
