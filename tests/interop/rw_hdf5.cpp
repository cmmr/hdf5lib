#include <R.h>
#include <Rinternals.h>
#include <stdio.h>
#include <stdlib.h>

#include <hdf5.h>
#include <hdf5_hl.h>
#include "hdf5lib.h"

#define H5Z_FILTER_BZIP2     307
#define H5Z_FILTER_SZIP      4
#define H5Z_FILTER_LZF       32000
#define H5Z_FILTER_BLOSC     32001
#define H5Z_FILTER_LZ4       32004
#define H5Z_FILTER_BSHUF     32008
#define H5Z_FILTER_ZFP       32013
#define H5Z_FILTER_ZSTD      32015
#define H5Z_FILTER_BLOSC2    32026

typedef struct {
  const char* name;
  H5Z_filter_t id;
  size_t nelmts;
  unsigned int cd_values[7];
} FilterConfig;

FilterConfig filters[27] = {
  {"zlibng_gzip", H5Z_FILTER_DEFLATE, 1, {9}},
  {"szip_ec",     H5Z_FILTER_SZIP,    2, {H5_SZIP_EC_OPTION_MASK, 8}},
  {"szip_nn",     H5Z_FILTER_SZIP,    2, {H5_SZIP_NN_OPTION_MASK, 8}},
  {"bzip2",       H5Z_FILTER_BZIP2,   1, {9}},
  {"lzf",         H5Z_FILTER_LZF,     0, {0}},
  {"lz4",         H5Z_FILTER_LZ4,     2, {0, 0}},
  {"lz4hc",       H5Z_FILTER_LZ4,     2, {0, 9}},
  {"zstd",        H5Z_FILTER_ZSTD,    1, {3}},
  {"bshuf_pure",  H5Z_FILTER_BSHUF,   2, {0, 0}},
  {"bshuf_lz4",   H5Z_FILTER_BSHUF,   2, {0, 2}},
  {"zfp_prec",    H5Z_FILTER_ZFP,     6, {2, 0, 16, 0, 0, 0}},
  {"zfp_rev",     H5Z_FILTER_ZFP,     6, {5, 0, 0, 0, 0, 0}},
  {"zfp_expert",  H5Z_FILTER_ZFP,     6, {4, 0, 1, 16, 16, 0}},
  {"blosc_lz",    H5Z_FILTER_BLOSC,   7, {0, 0, 0, 0, 5, 1, 0}},
  {"blosc_lz4",   H5Z_FILTER_BLOSC,   7, {0, 0, 0, 0, 5, 1, 1}},
  {"blosc_lz4hc", H5Z_FILTER_BLOSC,   7, {0, 0, 0, 0, 5, 1, 2}},
  {"blosc_snappy",H5Z_FILTER_BLOSC,   7, {0, 0, 0, 0, 5, 1, 3}},
  {"blosc_zlib",  H5Z_FILTER_BLOSC,   7, {0, 0, 0, 0, 5, 1, 4}},
  {"blosc_zstd",  H5Z_FILTER_BLOSC,   7, {0, 0, 0, 0, 5, 1, 5}},
  {"blosc2_lz",   H5Z_FILTER_BLOSC2,  7, {0, 0, 0, 0, 5, 1, 0}},
  {"blosc2_lz4",  H5Z_FILTER_BLOSC2,  7, {0, 0, 0, 0, 5, 1, 1}},
  {"blosc2_lz4hc",H5Z_FILTER_BLOSC2,  7, {0, 0, 0, 0, 5, 1, 2}},
  {"blosc2_snappy",H5Z_FILTER_BLOSC2, 7, {0, 0, 0, 0, 5, 1, 3}},
  {"blosc2_zlib", H5Z_FILTER_BLOSC2,  7, {0, 0, 0, 0, 5, 1, 4}},
  {"blosc2_zstd", H5Z_FILTER_BLOSC2,  7, {0, 0, 0, 0, 5, 1, 5}},
  {"blosc2_zfp",  H5Z_FILTER_BLOSC2,  7, {0, 0, 0, 0, 5, 1, 6}},
  {"blosc2_ndlz", H5Z_FILTER_BLOSC2,  7, {0, 0, 0, 0, 5, 1, 11}}
};

SEXP C_process_hdf5(SEXP r_file_in, SEXP r_file_out) {
    const char *file_in_name = CHAR(STRING_ELT(r_file_in, 0));
    const char *file_out_name = CHAR(STRING_ELT(r_file_out, 0));

    if (hdf5lib_register_all_filters() < 0) {
        Rf_error("Failed to register HDF5 plugins.");
    }

    H5Eset_auto(H5E_DEFAULT, NULL, NULL);

    hid_t file_in = H5Fopen(file_in_name, H5F_ACC_RDONLY, H5P_DEFAULT);
    if (file_in < 0) Rf_error("Failed to open input file: %s", file_in_name);
    
    hid_t file_out = H5Fcreate(file_out_name, H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
    if (file_out < 0) Rf_error("Failed to create output file: %s", file_out_name);

    for (int i = 0; i < 27; i++) {
        int next_i = (i + 1) % 27;
        
        double data[1024];
        if (H5LTread_dataset_double(file_in, filters[i].name, data) < 0) {
            Rf_error("Failed to READ dataset: %s", filters[i].name);
        }

        char out_name[128];
        snprintf(out_name, sizeof(out_name), "out_%s", filters[next_i].name);

        hsize_t dims[1] = {1024};
        hid_t space = H5Screate_simple(1, dims, NULL);
        if (space < 0) Rf_error("Failed to create dataspace for: %s", out_name);

        hid_t dcpl = H5Pcreate(H5P_DATASET_CREATE);
        if (dcpl < 0) Rf_error("Failed to create property list for: %s", out_name);

        hsize_t chunk[1] = {128};
        if (H5Pset_chunk(dcpl, 1, chunk) < 0) {
            Rf_error("Failed to set chunking for: %s", out_name);
        }
        
        /* Apply filters. Native wrappers automatically use H5Z_FLAG_OPTIONAL */
        if (filters[next_i].id == H5Z_FILTER_DEFLATE) {
            if (H5Pset_deflate(dcpl, filters[next_i].cd_values[0]) < 0)
                Rf_error("Failed to apply Deflate filter on: %s", out_name);
        } else if (filters[next_i].id == H5Z_FILTER_SZIP) {
            if (H5Pset_szip(dcpl, filters[next_i].cd_values[0], filters[next_i].cd_values[1]) < 0)
                Rf_error("Failed to apply SZIP filter on: %s", out_name);
        } else {
            if (H5Pset_filter(dcpl, filters[next_i].id, H5Z_FLAG_MANDATORY, filters[next_i].nelmts, filters[next_i].cd_values) < 0)
                Rf_error("Failed to apply filter %d on: %s", filters[next_i].id, out_name);
        }
        
        hid_t dset = H5Dcreate2(file_out, out_name, H5T_NATIVE_DOUBLE, space, H5P_DEFAULT, dcpl, H5P_DEFAULT);
        if (dset < 0) Rf_error("Failed to CREATE dataset: %s", out_name);
        
        if (H5Dwrite(dset, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, data) < 0) {
            Rf_error("Failed to WRITE dataset: %s", out_name);
        }
        
        if (H5Dclose(dset) < 0) Rf_error("Failed to close dataset: %s", out_name);
        if (H5Pclose(dcpl) < 0) Rf_error("Failed to close property list: %s", out_name);
        if (H5Sclose(space) < 0) Rf_error("Failed to close dataspace: %s", out_name);
    }

    H5Fclose(file_out);
    H5Fclose(file_in);
    hdf5lib_destroy_all_filters();
    
    return R_NilValue;
}
