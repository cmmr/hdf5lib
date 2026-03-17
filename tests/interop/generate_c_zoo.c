#include <R.h>
#include <Rinternals.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <hdf5.h>
#include <hdf5_hl.h>
#include "hdf5lib.h"

#define N_ELEMS 16384 // 128 * 128
#define CHUNK_X 128
#define CHUNK_Y 128

/* --- External ZFP Setters --- */
extern herr_t H5Pset_zfp_rate(hid_t plist, double rate);
extern herr_t H5Pset_zfp_precision(hid_t plist, unsigned int prec);
extern herr_t H5Pset_zfp_accuracy(hid_t plist, double acc);
extern herr_t H5Pset_zfp_expert(hid_t plist, unsigned int minbits, unsigned int maxbits, unsigned int maxprec, int minexp);
extern herr_t H5Pset_zfp_reversible(hid_t plist);

/* --- Filter IDs --- */
#define H5Z_FILTER_BZIP2     307
#define H5Z_FILTER_LZF       32000
#define H5Z_FILTER_BLOSC     32001
#define H5Z_FILTER_LZ4       32004
#define H5Z_FILTER_BSHUF     32008
#define H5Z_FILTER_ZFP       32013
#define H5Z_FILTER_ZSTD      32015
#define H5Z_FILTER_BLOSC2    32026

static void populate_float64(double *buf) {
    for (int i = 0; i < N_ELEMS; i++) buf[i] = (double)i / (N_ELEMS - 1) + sin((double)i * 50.0 / (N_ELEMS - 1)) * 0.1;
}
static void populate_float32(float *buf) {
    for (int i = 0; i < N_ELEMS; i++) buf[i] = (float)((double)i / (N_ELEMS - 1) + sin((double)i * 50.0 / (N_ELEMS - 1)) * 0.1);
}
static void populate_int64(long long *buf) { for (int i = 0; i < N_ELEMS; i++) buf[i] = i + 1; }
static void populate_int32(int *buf) { for (int i = 0; i < N_ELEMS; i++) buf[i] = i + 1; }
static void populate_int16(short *buf) { for (int i = 0; i < N_ELEMS; i++) buf[i] = (short)(i + 1); }

static void write_dset(hid_t file, hid_t space, const char* name, hid_t type, hid_t plist, void* data) {
    Rprintf("--> [START] Creating %-35s ... ", name);
    hid_t dset = H5Dcreate(file, name, type, space, H5P_DEFAULT, plist, H5P_DEFAULT);
    if (dset >= 0) {
        if (H5Dwrite(dset, type, H5S_ALL, H5S_ALL, H5P_DEFAULT, data) >= 0) Rprintf("DONE\n");
        else Rprintf("FAILED (Write Error)\n");
        H5Dclose(dset);
    } else Rprintf("FAILED (Create Error)\n");
}

SEXP C_write_zoo(SEXP sexp_filename) {
    const char *filename = CHAR(STRING_ELT(sexp_filename, 0));
    
    double *buf_f64 = malloc(N_ELEMS * sizeof(double));
    float *buf_f32 = malloc(N_ELEMS * sizeof(float));
    long long *buf_i64 = malloc(N_ELEMS * sizeof(long long));
    int *buf_i32 = malloc(N_ELEMS * sizeof(int));
    short *buf_i16 = malloc(N_ELEMS * sizeof(short));

    populate_float64(buf_f64); populate_float32(buf_f32);
    populate_int64(buf_i64); populate_int32(buf_i32); populate_int16(buf_i16);

    H5Eset_auto(H5E_DEFAULT, NULL, NULL);
    if (hdf5lib_register_all_filters() < 0) Rf_error("hdf5lib_register_all_filters() failed");

    hid_t file_id = H5Fcreate(filename, H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
    hsize_t dims[2] = {CHUNK_X, CHUNK_Y};
    hid_t space_id = H5Screate_simple(2, dims, NULL);
    hid_t plist;
    unsigned int cd_values[8];

    Rprintf("\n--- Generating %s ---\n", filename);

    // ==========================================
    // 1. Direct Lossless Plugins
    // ==========================================
    plist = H5Pcreate(H5P_DATASET_CREATE); H5Pset_chunk(plist, 2, dims);
    cd_values[0] = 9; H5Pset_filter(plist, H5Z_FILTER_BZIP2, H5Z_FLAG_MANDATORY, 1, cd_values);
    write_dset(file_id, space_id, "bzip2", H5T_NATIVE_LLONG, plist, buf_i64); H5Pclose(plist);

    plist = H5Pcreate(H5P_DATASET_CREATE); H5Pset_chunk(plist, 2, dims);
    cd_values[0] = 0; cd_values[1] = 0; H5Pset_filter(plist, H5Z_FILTER_LZ4, H5Z_FLAG_MANDATORY, 2, cd_values);
    write_dset(file_id, space_id, "lz4", H5T_NATIVE_FLOAT, plist, buf_f32); H5Pclose(plist);

    plist = H5Pcreate(H5P_DATASET_CREATE); H5Pset_chunk(plist, 2, dims);
    cd_values[0] = 0; cd_values[1] = 9; H5Pset_filter(plist, H5Z_FILTER_LZ4, H5Z_FLAG_MANDATORY, 2, cd_values);
    write_dset(file_id, space_id, "lz4hc", H5T_NATIVE_FLOAT, plist, buf_f32); H5Pclose(plist);

    plist = H5Pcreate(H5P_DATASET_CREATE); H5Pset_chunk(plist, 2, dims);
    H5Pset_filter(plist, H5Z_FILTER_LZF, H5Z_FLAG_MANDATORY, 0, NULL);
    write_dset(file_id, space_id, "lzf", H5T_NATIVE_INT, plist, buf_i32); H5Pclose(plist);

    plist = H5Pcreate(H5P_DATASET_CREATE); H5Pset_chunk(plist, 2, dims);
    cd_values[0] = 5; H5Pset_filter(plist, H5Z_FILTER_ZSTD, H5Z_FLAG_MANDATORY, 1, cd_values);
    write_dset(file_id, space_id, "zstd", H5T_NATIVE_SHORT, plist, buf_i16); H5Pclose(plist);

    // ==========================================
    // 2. Bitshuffle
    // ==========================================
    plist = H5Pcreate(H5P_DATASET_CREATE); H5Pset_chunk(plist, 2, dims);
    cd_values[0] = 0; cd_values[1] = 0; cd_values[2] = 0;
    H5Pset_filter(plist, H5Z_FILTER_BSHUF, H5Z_FLAG_MANDATORY, 3, cd_values);
    write_dset(file_id, space_id, "bitshuffle_none", H5T_NATIVE_INT, plist, buf_i32); H5Pclose(plist);

    plist = H5Pcreate(H5P_DATASET_CREATE); H5Pset_chunk(plist, 2, dims);
    cd_values[0] = 0; cd_values[1] = 2; cd_values[2] = 0;
    H5Pset_filter(plist, H5Z_FILTER_BSHUF, H5Z_FLAG_MANDATORY, 3, cd_values);
    write_dset(file_id, space_id, "bitshuffle_lz4", H5T_NATIVE_INT, plist, buf_i32); H5Pclose(plist);

    plist = H5Pcreate(H5P_DATASET_CREATE); H5Pset_chunk(plist, 2, dims);
    cd_values[0] = 0; cd_values[1] = 3; cd_values[2] = 5;
    H5Pset_filter(plist, H5Z_FILTER_BSHUF, H5Z_FLAG_MANDATORY, 3, cd_values);
    write_dset(file_id, space_id, "bitshuffle_zstd", H5T_NATIVE_LLONG, plist, buf_i64); H5Pclose(plist);

    // ==========================================
    // 3. ZFP Filters
    // ==========================================
    plist = H5Pcreate(H5P_DATASET_CREATE); H5Pset_chunk(plist, 2, dims);
    H5Pset_zfp_rate(plist, 16.0);
    write_dset(file_id, space_id, "zfp_rate", H5T_NATIVE_FLOAT, plist, buf_f32); H5Pclose(plist);

    plist = H5Pcreate(H5P_DATASET_CREATE); H5Pset_chunk(plist, 2, dims);
    H5Pset_zfp_precision(plist, 12);
    write_dset(file_id, space_id, "zfp_precision", H5T_NATIVE_DOUBLE, plist, buf_f64); H5Pclose(plist);

    plist = H5Pcreate(H5P_DATASET_CREATE); H5Pset_chunk(plist, 2, dims);
    H5Pset_zfp_accuracy(plist, 0.001);
    write_dset(file_id, space_id, "zfp_accuracy", H5T_NATIVE_FLOAT, plist, buf_f32); H5Pclose(plist);

    plist = H5Pcreate(H5P_DATASET_CREATE); H5Pset_chunk(plist, 2, dims);
    H5Pset_zfp_reversible(plist);
    write_dset(file_id, space_id, "zfp_reversible", H5T_NATIVE_INT, plist, buf_i32); H5Pclose(plist);

    plist = H5Pcreate(H5P_DATASET_CREATE); H5Pset_chunk(plist, 2, dims);
    H5Pset_zfp_expert(plist, 1, 16657, 64, -1074);
    write_dset(file_id, space_id, "zfp_expert", H5T_NATIVE_DOUBLE, plist, buf_f64); H5Pclose(plist);

    // ==========================================
    // 4. Exhaustive Blosc1 Matrix
    // ==========================================
    const char* b1_codecs[] = {"blosclz", "lz4", "lz4hc", "snappy", "zlib", "zstd"};
    int b1_compcodes[] = {0, 1, 2, 3, 4, 5};
    const char* b1_filters[] = {"noshuffle", "shuffle", "bitshuffle"};
    int b1_filter_ids[] = {0, 1, 2};

    for (int c = 0; c < 6; c++) {
        for (int f = 0; f < 3; f++) {
            char name[128];
            snprintf(name, sizeof(name), "blosc_%s_%s", b1_codecs[c], b1_filters[f]);
            
            hid_t dtype = (b1_filter_ids[f] == 2) ? H5T_NATIVE_DOUBLE : H5T_NATIVE_INT;
            void* dbuf = (b1_filter_ids[f] == 2) ? (void*)buf_f64 : (void*)buf_i32;

            plist = H5Pcreate(H5P_DATASET_CREATE); H5Pset_chunk(plist, 2, dims);
            unsigned int cd[] = {0, 0, 0, 0, 5, b1_filter_ids[f], b1_compcodes[c]}; 
            H5Pset_filter(plist, H5Z_FILTER_BLOSC, H5Z_FLAG_MANDATORY, 7, cd);
            
            write_dset(file_id, space_id, name, dtype, plist, dbuf); 
            H5Pclose(plist);
        }
    }

    // ==========================================
    // 5. Exhaustive Blosc2 Matrix
    // ==========================================
    const char* b2_codecs[] = {"blosclz", "lz4", "lz4hc", "zlib", "zstd"};
    int b2_compcodes[] = {0, 1, 2, 4, 5};
    const char* b2_filters[] = {"nofilter", "shuffle", "bitshuffle", "delta", "truncprec"};
    int b2_filter_ids[] = {0, 1, 2, 3, 4};

    for (int c = 0; c < 5; c++) {
        for (int f = 0; f < 5; f++) {
            char name[128];
            snprintf(name, sizeof(name), "blosc2_%s_%s", b2_codecs[c], b2_filters[f]);
            
            hid_t dtype = H5T_NATIVE_INT;
            void* dbuf = buf_i32;
            int meta = 0;
            
            // TruncPrec strictly requires floating point data
            if (b2_filter_ids[f] == 4) {
                dtype = H5T_NATIVE_FLOAT;
                dbuf = buf_f32;
                meta = 16; // Demand 16-bit precision to keep values within verify_c_zoo bounds
            } 
            // Bitshuffle favors 64-bit alignment
            else if (b2_filter_ids[f] == 2) { 
                dtype = H5T_NATIVE_DOUBLE; 
                dbuf = buf_f64;
            } 
            // Nofilter or Delta with smaller ints
            else if (b2_filter_ids[f] == 0) {
                dtype = H5T_NATIVE_SHORT;  
                dbuf = buf_i16;
            }

            plist = H5Pcreate(H5P_DATASET_CREATE); H5Pset_chunk(plist, 2, dims);
            
            // cd_values mapping: [0..3] metadata, [4] clevel, [5] filter, [6] compcode, [7] meta
            unsigned int cd[] = {0, 0, 0, 0, 5, b2_filter_ids[f], b2_compcodes[c], meta}; 
            H5Pset_filter(plist, H5Z_FILTER_BLOSC2, H5Z_FLAG_MANDATORY, 8, cd);
            
            write_dset(file_id, space_id, name, dtype, plist, dbuf); 
            H5Pclose(plist);
        }
    }

    // ==========================================
    // 6. Native Built-ins
    // ==========================================
    plist = H5Pcreate(H5P_DATASET_CREATE); H5Pset_chunk(plist, 2, dims);
    H5Pset_deflate(plist, 9);
    write_dset(file_id, space_id, "gzip_deflate", H5T_NATIVE_INT, plist, buf_i32); H5Pclose(plist);

    plist = H5Pcreate(H5P_DATASET_CREATE); H5Pset_chunk(plist, 2, dims);
    H5Pset_szip(plist, H5_SZIP_EC_OPTION_MASK, 8); // 128 is cleanly divisible by 8
    write_dset(file_id, space_id, "szip_ec", H5T_NATIVE_INT, plist, buf_i32); H5Pclose(plist);

    plist = H5Pcreate(H5P_DATASET_CREATE); H5Pset_chunk(plist, 2, dims);
    H5Pset_szip(plist, H5_SZIP_NN_OPTION_MASK, 8);
    write_dset(file_id, space_id, "szip_nn", H5T_NATIVE_INT, plist, buf_i32); H5Pclose(plist);

    // Cleanup
    H5Sclose(space_id); H5Fclose(file_id); hdf5lib_destroy_all_filters();
    free(buf_f64); free(buf_f32); free(buf_i64); free(buf_i32); free(buf_i16);

    return Rf_ScalarInteger(1);
}
