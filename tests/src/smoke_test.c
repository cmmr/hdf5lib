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
#define TEST_FLT 1
#define TEST_CMP 2
#define TEST_ARR 4
#define TEST_ALL (TEST_FLT | TEST_CMP | TEST_ARR)

/* --- Verbose Error Macro --- */
#define CHECK(expr, step_msg) do { \
  if ((expr) < 0) { \
    Rprintf("\n      -> %s failed", step_msg); \
    goto cleanup; \
  } \
} while(0)

/* --- Filter Configuration Architecture --- */
typedef struct {
  const char*  name;
  H5Z_filter_t id;
  size_t       nelmts;
  unsigned int cd_values[7];
  int          test_mask;
} FilterConfig;

/* Helper to apply filter natively */
static herr_t apply_filter(hid_t plist, const FilterConfig* cfg) {
  if (cfg->id == H5Z_FILTER_DEFLATE) {
    return H5Pset_deflate(plist, cfg->cd_values[0]);
  } else if (cfg->id == H5Z_FILTER_SZIP) {
    return H5Pset_szip(plist, cfg->cd_values[0], cfg->cd_values[1]);
  } else {
    /* Use H5Z_FLAG_OPTIONAL so incompressible chunks don't fail the pipeline */
    return H5Pset_filter(plist, cfg->id, H5Z_FLAG_OPTIONAL, cfg->nelmts, cfg->cd_values);
  }
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

  for(int i=0; i<FLT_SIZE; i++) { data[i] = (float)i; data_out[i] = -1.0; }

  CHECK(sid = H5Screate_simple(1, dims, NULL), "H5Screate_simple");
  CHECK(plist = H5Pcreate(H5P_DATASET_CREATE), "H5Pcreate");
  CHECK(H5Pset_chunk(plist, 1, chunkdims), "H5Pset_chunk");
  CHECK(apply_filter(plist, cfg), "apply_filter");

  char dset_name[128];
  snprintf(dset_name, sizeof(dset_name), "/float_%s", cfg->name);
  CHECK(dset = H5Dcreate2(fid, dset_name, H5T_NATIVE_FLOAT, sid, H5P_DEFAULT, plist, H5P_DEFAULT), "H5Dcreate2");
  CHECK(H5Dwrite(dset, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, H5P_DEFAULT, data), "H5Dwrite");
  CHECK(H5Dread(dset, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, H5P_DEFAULT, data_out), "H5Dread");

  /* Use a small epsilon for lossy ZFP comparisons */
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

/* 2. Compound Data Test (> 255 bytes to test Blosc fallback) */
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

/* 3. Array Data Test */
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
    {"szip_ec",     H5Z_FILTER_SZIP,    2, {H5_SZIP_EC_OPTION_MASK, 8},   TEST_FLT},
    {"szip_nn",     H5Z_FILTER_SZIP,    2, {H5_SZIP_NN_OPTION_MASK, 8},   TEST_FLT},
    {"bzip2",       H5Z_FILTER_BZIP2,   1, {9},                           TEST_ALL},
    {"lzf",         H5Z_FILTER_LZF,     0, {0},                           TEST_ALL},
    {"lz4",         H5Z_FILTER_LZ4,     2, {0, 0},                        TEST_ALL},
    {"lz4hc",       H5Z_FILTER_LZ4,     2, {0, 9},                        TEST_ALL},
    {"zstd",        H5Z_FILTER_ZSTD,    1, {3},                           TEST_ALL},
    {"snappy",      H5Z_FILTER_SNAPPY,  0, {0},                           TEST_ALL},
    {"bshuf_pure",  H5Z_FILTER_BSHUF,   2, {0, 0},                        TEST_ALL},
    {"bshuf_lz4",   H5Z_FILTER_BSHUF,   2, {0, 2},                        TEST_ALL},
    {"zfp_prec",    H5Z_FILTER_ZFP,     6, {2, 0, 16, 0, 0, 0},           TEST_FLT},
    {"zfp_rev",     H5Z_FILTER_ZFP,     6, {5, 0, 0, 0, 0, 0},            TEST_FLT},
    {"zfp_expert",  H5Z_FILTER_ZFP,     6, {4, 0, 1, 16, 16, 0},          TEST_FLT},
    
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
    {"blosc2_zfp",  H5Z_FILTER_BLOSC2,  7, {0, 0, 0, 0, 5, 1, 6},         TEST_FLT}, 
    {"blosc2_ndlz", H5Z_FILTER_BLOSC2,  7, {0, 0, 0, 0, 5, 1, 11},        TEST_ALL}
  };
  
  int num_filters = sizeof(filters) / sizeof(FilterConfig);

  Rprintf("Running Strict HDF5 Plugin Smoke Tests...\n");
  for (int i = 0; i < num_filters; i++) {
    Rprintf("  Testing %-15s", filters[i].name);
    
    int err = 0;
    
    /* Execute expected combinations */
    if (filters[i].test_mask & TEST_FLT) { if (test_float_data(file_id, &filters[i])    < 0) err++; }
    if (filters[i].test_mask & TEST_CMP) { if (test_compound_data(file_id, &filters[i]) < 0) err++; }
    if (filters[i].test_mask & TEST_ARR) { if (test_array_data(file_id, &filters[i])    < 0) err++; }
    
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
