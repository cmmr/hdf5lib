// [[Rcpp::depends(hdf5lib)]]
#include <Rcpp.h>
#include <hdf5.h>
#include <hdf5_hl.h>
#include "hdf5lib.h"

using namespace Rcpp;

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

struct FilterConfig {
  const char* name;
  H5Z_filter_t id;
  size_t nelmts;
  unsigned int cd_values[7];
};

FilterConfig filters[28] = {
  {"zlibng_gzip", H5Z_FILTER_DEFLATE, 1, {9}},
  {"szip_ec",     H5Z_FILTER_SZIP,    2, {H5_SZIP_EC_OPTION_MASK, 8}},
  {"szip_nn",     H5Z_FILTER_SZIP,    2, {H5_SZIP_NN_OPTION_MASK, 8}},
  {"bzip2",       H5Z_FILTER_BZIP2,   1, {9}},
  {"lzf",         H5Z_FILTER_LZF,     0, {0}},
  {"lz4",         H5Z_FILTER_LZ4,     2, {0, 0}},
  {"lz4hc",       H5Z_FILTER_LZ4,     2, {0, 9}},
  {"zstd",        H5Z_FILTER_ZSTD,    1, {3}},
  {"snappy",      H5Z_FILTER_SNAPPY,  0, {0}},
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

// [[Rcpp::export]]
void process_hdf5() {
    hdf5lib_register_all_filters();

    hid_t file_in = H5Fopen("python_out.h5", H5F_ACC_RDONLY, H5P_DEFAULT);
    hid_t file_out = H5Fcreate("r_out.h5", H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);

    for (int i = 0; i < 28; i++) {
        int next_i = (i + 1) % 28;
        
        std::vector<double> data(1024);
        H5LTread_dataset_double(file_in, filters[i].name, data.data());

        char out_name[128];
        snprintf(out_name, sizeof(out_name), "out_%s", filters[next_i].name);

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
        H5Dwrite(dset, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, data.data());
        
        H5Dclose(dset);
        H5Pclose(dcpl);
        H5Sclose(space);
    }

    H5Fclose(file_out);
    H5Fclose(file_in);
    hdf5lib_destroy_all_filters();
}
