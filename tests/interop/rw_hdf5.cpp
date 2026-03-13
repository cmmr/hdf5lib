// [[Rcpp::depends(hdf5lib)]]
#include <Rcpp.h>
#include <vector>

#include <hdf5.h>
#include <hdf5_hl.h>
#include "hdf5lib.h"

using namespace Rcpp;

#define H5Z_FILTER_BZIP2     307
#define H5Z_FILTER_SZIP      4
#define H5Z_FILTER_LZF       32000
#define H5Z_FILTER_BLOSC     32001
#define H5Z_FILTER_LZ4       32004
#define H5Z_FILTER_BSHUF     32008
#define H5Z_FILTER_ZFP       32013
#define H5Z_FILTER_ZSTD      32015
#define H5Z_FILTER_BLOSC2    32026

struct FilterConfig {
  const char* name;
  H5Z_filter_t id;
  size_t nelmts;
  unsigned int cd_values[7];
};

// Reduced to 23 exact matches of Python's capabilities
FilterConfig filters[23] = {
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
  {"blosc_zlib",  H5Z_FILTER_BLOSC,   7, {0, 0, 0, 0, 5, 1, 4}},
  {"blosc_zstd",  H5Z_FILTER_BLOSC,   7, {0, 0, 0, 0, 5, 1, 5}},
  {"blosc2_lz",   H5Z_FILTER_BLOSC2,  7, {0, 0, 0, 0, 5, 1, 0}},
  {"blosc2_lz4",  H5Z_FILTER_BLOSC2,  7, {0, 0, 0, 0, 5, 1, 1}},
  {"blosc2_lz4hc",H5Z_FILTER_BLOSC2,  7, {0, 0, 0, 0, 5, 1, 2}},
  {"blosc2_zlib", H5Z_FILTER_BLOSC2,  7, {0, 0, 0, 0, 5, 1, 4}},
  {"blosc2_zstd", H5Z_FILTER_BLOSC2,  7, {0, 0, 0, 0, 5, 1, 5}}
};

// [[Rcpp::export]]
void process_hdf5() {
    if (hdf5lib_register_all_filters() < 0) stop("Failed to register HDF5 plugins.");
    
    H5Eset_auto(H5E_DEFAULT, NULL, NULL);

    hid_t file_in = H5Fopen("python_out.h5", H5F_ACC_RDONLY, H5P_DEFAULT);
    if (file_in < 0) stop("Failed to open input file.");
    
    hid_t file_out = H5Fcreate("r_out.h5", H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
    if (file_out < 0) stop("Failed to create output file.");

    for (int i = 0; i < 23; i++) {
        int next_i = (i + 1) % 23;
        
        char out_name[128];
        snprintf(out_name, sizeof(out_name), "out_%s", filters[next_i].name);

        std::vector<double> data(1024);
        if (H5LTread_dataset_double(file_in, filters[i].name, data.data()) < 0) {
            stop("Failed to READ dataset: %s", filters[i].name);
        }

        hsize_t dims[1] = {1024};
        hid_t space = H5Screate_simple(1, dims, NULL);
        hid_t dcpl = H5Pcreate(H5P_DATASET_CREATE);
        hsize_t chunk[1] = {128};
        H5Pset_chunk(dcpl, 1, chunk);
        
        if (filters[next_i].id == H5Z_FILTER_DEFLATE) {
            H5Pset_deflate(dcpl, filters[next_i].cd_values[0]);
        } else if (filters[next_i].id == H5Z_FILTER_SZIP) {
            H5Pset_szip(dcpl, filters[next_i].cd_values[0], filters[next_i].cd_values[1]);
        } else {
            H5Pset_filter(dcpl, filters[next_i].id, H5Z_FLAG_MANDATORY, filters[next_i].nelmts, filters[next_i].cd_values);
        }
        
        hid_t dset = H5Dcreate2(file_out, out_name, H5T_NATIVE_DOUBLE, space, H5P_DEFAULT, dcpl, H5P_DEFAULT);
        if (H5Dwrite(dset, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, data.data()) < 0) {
            stop("Failed to WRITE dataset: %s", out_name);
        }
        
        if (H5Dclose(dset) < 0) stop("Failed to close dataset: %s", out_name);
        H5Pclose(dcpl);
        H5Sclose(space);
    }

    H5Fclose(file_out);
    H5Fclose(file_in);
    hdf5lib_destroy_all_filters();
}
