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

/* Blosc2's cd_values[0]: tag 0xB2, layout 1, the pre-filter bitmask (1
   shuffle, 2 bitshuffle, 4 delta, 8 trunc-prec), the meta value, and the
   reference filter's revision, 1 */
#define B2_SETTINGS(mask, meta) ((0xB2u << 24) | (1u << 20) | (((mask) & 0xFu) << 16) | \
                                 (((meta) & 0xFFu) << 8) | 0x01u)

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

/* One Blosc2 dataset per codec x pre-filter, plus Blosc2's ZFP codecs and NDLZ,
   in the given chunk geometry.  The plugin records cd_values differently by
   rank -- B2ND geometry for 2-D chunks, none for 1-D chunks or ranks beyond
   B2ND's limit -- and h5py has to read every one of them.  Axis 0 is
   unlimited, so that h5py can append to them (check_hdf5plugin_append.py). */
static void write_blosc2_matrix(hid_t file_id, const char *prefix, int rank, const hsize_t *dims,
                                const hsize_t *chunk, double *buf_f64, float *buf_f32,
                                int *buf_i32, short *buf_i16) {
    hsize_t maxdims[H5S_MAX_RANK];
    for (int i = 0; i < rank; i++) maxdims[i] = dims[i];
    maxdims[0] = H5S_UNLIMITED;
    hid_t space_id = H5Screate_simple(rank, dims, maxdims);
    hid_t plist;
    const char* b2_codecs[] = {"blosclz", "lz4", "lz4hc", "zlib", "zstd"};
    int b2_compcodes[] = {0, 1, 2, 4, 5};
    const char* b2_filters[] = {"nofilter", "shuffle", "bitshuffle", "delta", "truncprec"};
    unsigned int b2_filter_masks[] = {0, 1, 2, 4, 8};  /* the bitmask in cd_values[0] */
    unsigned int b2_filter_codes[] = {0, 1, 2, 3, 4};  /* the same, as cd_values[5] names it */

    for (int c = 0; c < 5; c++) {
        for (int f = 0; f < 5; f++) {
            char name[128];
            snprintf(name, sizeof(name), "%sblosc2_%s_%s", prefix, b2_codecs[c], b2_filters[f]);

            hid_t dtype = H5T_NATIVE_INT;
            void* dbuf = buf_i32;
            unsigned int meta = 0;

            // TruncPrec strictly requires floating point data
            if (b2_filter_masks[f] == 8) {
                dtype = H5T_NATIVE_FLOAT;
                dbuf = buf_f32;
                meta = 16; // Demand 16-bit precision to keep values within verify_c_zoo bounds
            }
            // Bitshuffle favors 64-bit alignment
            else if (b2_filter_masks[f] == 2) {
                dtype = H5T_NATIVE_DOUBLE;
                dbuf = buf_f64;
            }
            // Nofilter or Delta with smaller ints
            else if (b2_filter_masks[f] == 0) {
                dtype = H5T_NATIVE_SHORT;
                dbuf = buf_i16;
            }

            plist = H5Pcreate(H5P_DATASET_CREATE); H5Pset_chunk(plist, rank, chunk);

            // cd_values: [0] packed settings, [1..3] filled in, [4] clevel, [5] filter, [6] compcode
            unsigned int cd[] = {B2_SETTINGS(b2_filter_masks[f], meta), 0, 0, 0, 5,
                                 b2_filter_codes[f], b2_compcodes[c]};
            H5Pset_filter(plist, H5Z_FILTER_BLOSC2, H5Z_FLAG_MANDATORY, 7, cd);

            write_dset(file_id, space_id, name, dtype, plist, dbuf);
            H5Pclose(plist);
        }
    }

    /* ZFP (lossy, so float64) takes its parameter as the meta value; NDLZ its cell size */
    const char* extra_names[] = {"zfp_accuracy", "zfp_precision", "zfp_rate", "ndlz"};
    unsigned int extra_compcodes[] = {33, 34, 35, 32};
    unsigned int extra_meta[] = {253, 16, 25, 4};  /* 10^-3, 16 bits, 25% of 64 bits, 4x4 cells */
    for (int e = 0; e < 4; e++) {
        char name[128];
        snprintf(name, sizeof(name), "%sblosc2_%s", prefix, extra_names[e]);
        int is_ndlz = (extra_compcodes[e] == 32);
        plist = H5Pcreate(H5P_DATASET_CREATE); H5Pset_chunk(plist, rank, chunk);
        unsigned int cd[] = {B2_SETTINGS(0, extra_meta[e]), 0, 0, 0, 5, 0, extra_compcodes[e]};
        H5Pset_filter(plist, H5Z_FILTER_BLOSC2, H5Z_FLAG_MANDATORY, 7, cd);
        write_dset(file_id, space_id, name, is_ndlz ? H5T_NATIVE_INT : H5T_NATIVE_DOUBLE, plist,
                   is_ndlz ? (void*)buf_i32 : (void*)buf_f64);
        H5Pclose(plist);
    }

    H5Sclose(space_id);
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
    hsize_t ext_maxdims[2] = {H5S_UNLIMITED, CHUNK_Y};  /* Blosc2 datasets h5py appends to */
    hid_t ext_space_id = H5Screate_simple(2, dims, ext_maxdims);
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
    // 5. Exhaustive Blosc2 Matrix: 2-D, 1-D, and a rank beyond B2ND's limit
    // ==========================================
    write_blosc2_matrix(file_id, "", 2, dims, dims, buf_f64, buf_f32, buf_i32, buf_i16);

    hsize_t dims_1d[1] = {N_ELEMS}, chunk_1d[1] = {N_ELEMS / 4};
    write_blosc2_matrix(file_id, "1d_", 1, dims_1d, chunk_1d, buf_f64, buf_f32, buf_i32, buf_i16);

    hsize_t dims_17d[17];
    for (int i = 0; i < 17; i++) dims_17d[i] = 1;
    dims_17d[0] = CHUNK_X; dims_17d[1] = CHUNK_Y;
    write_blosc2_matrix(file_id, "17d_", 17, dims_17d, dims_17d, buf_f64, buf_f32, buf_i32, buf_i16);

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

    // ==========================================
    // 7. Blosc2 Multi-Filter Pipeline
    // ==========================================
    // Delta, then bitshuffle: the bitmask in cd_values[0], the last stage in
    // cd_values[5].  Every chunk records its whole pipeline, so standard h5py
    // reads it.
    plist = H5Pcreate(H5P_DATASET_CREATE);
    H5Pset_chunk(plist, 2, dims);

    unsigned int cd_pipeline[7] = {0};
    cd_pipeline[0] = B2_SETTINGS(4 | 2, 0); // DELTA (4) + BITSHUFFLE (2) = 0xB2160001
    cd_pipeline[4] = 1; // Compression level (1 = ~3 in Zstd's range)
    cd_pipeline[5] = 2; // BITSHUFFLE, the last stage
    cd_pipeline[6] = 5; // Compressor code (ZSTD = 5)

    H5Pset_filter(plist, H5Z_FILTER_BLOSC2, H5Z_FLAG_MANDATORY, 7, cd_pipeline);
    write_dset(file_id, ext_space_id, "blosc2_delta_bshuf_zstd", H5T_NATIVE_INT, plist, buf_i32);
    H5Pclose(plist);

    // The same two, and trunc-prec(16) + bitshuffle, in the form hdf5lib took
    // before (cd_values[0] = 0): the bitmask in cd_values[5], the precision as
    // an 8th value.  The plugin stores them exactly as the new form.
    plist = H5Pcreate(H5P_DATASET_CREATE); H5Pset_chunk(plist, 2, dims);
    unsigned int cd_old_pipeline[7] = {0, 0, 0, 0, 1, 4 | 2, 5};
    H5Pset_filter(plist, H5Z_FILTER_BLOSC2, H5Z_FLAG_MANDATORY, 7, cd_old_pipeline);
    write_dset(file_id, ext_space_id, "blosc2_oldform_delta_bshuf_zstd", H5T_NATIVE_INT, plist, buf_i32);
    H5Pclose(plist);

    plist = H5Pcreate(H5P_DATASET_CREATE); H5Pset_chunk(plist, 2, dims);
    unsigned int cd_old_trunc[8] = {0, 0, 0, 0, 5, 8 | 2, 1, 16};
    H5Pset_filter(plist, H5Z_FILTER_BLOSC2, H5Z_FLAG_MANDATORY, 8, cd_old_trunc);
    write_dset(file_id, ext_space_id, "blosc2_oldform_truncprec_bshuf_lz4", H5T_NATIVE_FLOAT, plist, buf_f32);
    H5Pclose(plist);

    // ==========================================
    // 8. Optional ZFP on Chunks It Cannot Take
    // ==========================================
    // A single value and a 5-D chunk: ZFP records values that refuse every
    // chunk, so each one is stored unfiltered and h5py reads it without ZFP.
    hsize_t dims_single[1] = {1}, dims_5d[5] = {2, 2, 2, 2, 2};
    const char* declined_names[] = {"zfp_declined_single", "zfp_declined_5d"};
    const hsize_t* declined_dims[] = {dims_single, dims_5d};
    int declined_ranks[] = {1, 5};
    for (int d = 0; d < 2; d++) {
        hid_t declined_space = H5Screate_simple(declined_ranks[d], declined_dims[d], NULL);
        plist = H5Pcreate(H5P_DATASET_CREATE); H5Pset_chunk(plist, declined_ranks[d], declined_dims[d]);
        H5Pset_zfp_reversible(plist);
        H5Pmodify_filter(plist, H5Z_FILTER_ZFP, H5Z_FLAG_OPTIONAL, 0, NULL);
        write_dset(file_id, declined_space, declined_names[d], H5T_NATIVE_INT, plist, buf_i32);
        H5Pclose(plist); H5Sclose(declined_space);
    }

    // Blosc2's ZFP codecs take native floats only: on int32 an optional filter
    // records compression level 255, so every chunk is stored unfiltered.
    plist = H5Pcreate(H5P_DATASET_CREATE); H5Pset_chunk(plist, 2, dims);
    unsigned int cd_b2zfp[7] = {B2_SETTINGS(0, 253), 0, 0, 0, 5, 0, 33};  // accuracy 10^-3
    H5Pset_filter(plist, H5Z_FILTER_BLOSC2, H5Z_FLAG_OPTIONAL, 7, cd_b2zfp);
    write_dset(file_id, space_id, "blosc2_zfp_declined_int32", H5T_NATIVE_INT, plist, buf_i32);
    H5Pclose(plist);

    // Cleanup
    H5Sclose(space_id); H5Sclose(ext_space_id); H5Fclose(file_id); hdf5lib_destroy_all_filters();
    free(buf_f64); free(buf_f32); free(buf_i64); free(buf_i32); free(buf_i16);

    return Rf_ScalarInteger(1);
}
