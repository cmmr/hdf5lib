/* Used by test-exit.r.  A child R process calls C_exit_test_write(): it writes
   with the Blosc and Blosc2 filters on two threads, leaves the thread count at
   two, and leaves a dataset of each open with chunks still in HDF5's chunk
   cache, so that HDF5 compresses those as the process exits.  The test then
   checks that the child exited, and C_exit_test_verify() reads everything
   back. */

#include <R.h>
#include <Rinternals.h>
#include <hdf5.h>
#include <hdf5lib.h>

#define H5Z_FILTER_BLOSC  32001

#define N      (1 << 19)   /* 4 MB of doubles per dataset */
#define CHUNK  (1 << 16)   /* 512 KB chunks: several Blosc blocks each, and
                              small enough to stay in the 1 MB chunk cache */

static const char *const names[] = { "/b1", "/b2", "/b1_open", "/b2_open" };

static double value(int i) { return (double)(i % 1000) * 0.5; }

static hid_t write_dset(hid_t fid, const char *name, int blosc2, const double *data) {
  hsize_t dims = N, chunk = CHUNK;
  hid_t space = H5Screate_simple(1, &dims, NULL);
  hid_t dcpl  = H5Pcreate(H5P_DATASET_CREATE);
  H5Pset_chunk(dcpl, 1, &chunk);
  if (blosc2) {
    hdf5lib_set_blosc2(dcpl, H5Z_FLAG_MANDATORY, HDF5LIB_BLOSC2_LZ4, 5, HDF5LIB_BLOSC2_SHUFFLE, 0);
  } else {
    unsigned int cd[7] = {0, 0, 0, 0, 5, 1, 1};   /* level 5, shuffle, LZ4 */
    H5Pset_filter(dcpl, H5Z_FILTER_BLOSC, H5Z_FLAG_MANDATORY, 7, cd);
  }
  hid_t dset = H5Dcreate2(fid, name, H5T_NATIVE_DOUBLE, space, H5P_DEFAULT, dcpl, H5P_DEFAULT);
  H5Pclose(dcpl);
  H5Sclose(space);
  if (dset < 0 || H5Dwrite(dset, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, data) < 0)
    Rf_error("C_exit_test_write: could not write %s", name);
  return dset;
}

static int read_ok(hid_t fid, const char *name, double *buf) {
  hid_t dset = H5Dopen2(fid, name, H5P_DEFAULT);
  if (dset < 0) return 0;
  herr_t rc = H5Dread(dset, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, buf);
  H5Dclose(dset);
  if (rc < 0) return 0;
  for (int i = 0; i < N; i++)
    if (buf[i] != value(i)) return 0;
  return 1;
}

SEXP C_exit_test_write(SEXP sexp_filename) {
  if (hdf5lib_register_all_filters() < 0)
    Rf_error("C_exit_test_write: hdf5lib_register_all_filters() failed");
  hdf5lib_set_blosc_nthreads(2);

  double *data = (double *)R_alloc(N, sizeof(double));
  double *buf  = (double *)R_alloc(N, sizeof(double));
  for (int i = 0; i < N; i++) data[i] = value(i);

  hid_t fid = H5Fcreate(CHAR(STRING_ELT(sexp_filename, 0)), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
  if (fid < 0) Rf_error("C_exit_test_write: H5Fcreate failed");

  /* Use both directions of both filters on two threads... */
  for (int f = 0; f < 2; f++) {
    H5Dclose(write_dset(fid, names[f], f, data));
    if (!read_ok(fid, names[f], buf))
      Rf_error("C_exit_test_write: %s did not read back", names[f]);
  }

  /* ...then leave these, and the file, for HDF5 to flush as R exits */
  write_dset(fid, names[2], 0, data);
  write_dset(fid, names[3], 1, data);
  return R_NilValue;
}

SEXP C_exit_test_verify(SEXP sexp_filename) {
  if (hdf5lib_register_all_filters() < 0)
    Rf_error("C_exit_test_verify: hdf5lib_register_all_filters() failed");

  double *buf = (double *)R_alloc(N, sizeof(double));
  hid_t fid = H5Fopen(CHAR(STRING_ELT(sexp_filename, 0)), H5F_ACC_RDONLY, H5P_DEFAULT);
  if (fid < 0) Rf_error("C_exit_test_verify: H5Fopen failed");

  /* Whether each of names[] read back intact */
  SEXP ok = PROTECT(Rf_allocVector(LGLSXP, 4));
  for (int i = 0; i < 4; i++) LOGICAL(ok)[i] = read_ok(fid, names[i], buf);
  H5Fclose(fid);
  UNPROTECT(1);
  return ok;
}
