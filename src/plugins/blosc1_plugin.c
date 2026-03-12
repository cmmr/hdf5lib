/**
 * @file blosc_plugin.c
 * @brief Standalone HDF5 Filter Plugin for Blosc
 */

#include <hdf5.h>
#include <blosc2.h>
#include <stdlib.h>
#include <string.h>

#define H5Z_FILTER_BLOSC 32001
#define FILTER_BLOSC_VERSION 2
#define BLOSC_MAX_TYPESIZE 255

#define PUSH_ERR(...) do { \
    H5Epush(H5E_DEFAULT, __FILE__, __func__, __LINE__, H5E_ERR_CLS, H5E_PLINE, H5E_CANTFILTER, __VA_ARGS__); \
    return 0; \
} while(0)

static herr_t blosc_set_local(hid_t dcpl, hid_t type, hid_t space) {
  unsigned int flags, values[8];
  size_t nelem = 8, typesize;
  if (H5Pget_filter_by_id(dcpl, H5Z_FILTER_BLOSC, &flags, &nelem, values, 0, NULL, NULL) < 0) return -1;

  typesize = H5Tget_size(type);
  if (H5Tget_class(type) == H5T_ARRAY) {
    hid_t st = H5Tget_super(type);
    typesize = H5Tget_size(st);
    H5Tclose(st);
  }
  values[2] = (typesize > BLOSC_MAX_TYPESIZE) ? 1 : (unsigned int)typesize;

  hsize_t dims[32];
  int nd = H5Pget_chunk(dcpl, 32, dims);
  size_t csz = H5Tget_size(type);
  for (int i = 0; i < nd; i++) csz *= dims[i];
  values[3] = (unsigned int)csz;

  return H5Pmodify_filter(dcpl, H5Z_FILTER_BLOSC, flags, nelem, values);
}

static size_t blosc_filter(unsigned int flags, size_t cd_nelmts, const unsigned int cd_values[], size_t nbytes, size_t *buf_size, void **buf) {
  if (nbytes == 0) return 0;

  if (flags & H5Z_FLAG_REVERSE) {
    size_t nchunks, screensize, utypesize;
    blosc_cbuffer_sizes(*buf, &nchunks, &screensize, &utypesize);
    void *outbuf = H5allocate_memory(nchunks, 0);
    if (!outbuf) PUSH_ERR("Memory error");
    if (blosc_decompress(*buf, outbuf, nchunks) <= 0) { H5free_memory(outbuf); PUSH_ERR("Decompress error"); }
    H5free_memory(*buf); *buf = outbuf; *buf_size = nchunks;
    return nchunks;
  } else {
    size_t typesize = cd_values[2];
    int clevel = (cd_nelmts >= 5) ? (int)cd_values[4] : 5;
    int shuffle = (cd_nelmts >= 6) ? (int)cd_values[5] : 1;
    const char* cname = "blosclz";
    if (cd_nelmts >= 7) blosc_compcode_to_compname(cd_values[6], &cname);

    void *outbuf = H5allocate_memory(nbytes, 0);
    if (!outbuf) PUSH_ERR("Memory error");
    blosc_set_compressor(cname);
    int csz = blosc_compress(clevel, shuffle, typesize, nbytes, *buf, outbuf, nbytes);

    /* IF INCOMPRESSIBLE: Free outbuf and destroy Blosc state before returning 0 */
    if (csz <= 0) {
      H5free_memory(outbuf);
      blosc_destroy(); 
      return 0;
    }
    H5free_memory(*buf); *buf = outbuf; *buf_size = nbytes;
    return (size_t)csz;
  }
}

const H5Z_class2_t blosc_class = { H5Z_CLASS_T_VERS, H5Z_FILTER_BLOSC, 1, 1, "blosc", NULL, blosc_set_local, blosc_filter };
