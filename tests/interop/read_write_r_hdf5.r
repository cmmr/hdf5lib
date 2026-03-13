library(Rcpp)

Sys.setenv(PKG_CPPFLAGS = hdf5lib::c_flags(api = 2.0))
Sys.setenv(PKG_LIBS = hdf5lib::ld_flags(api = 2.0))

message("Compiling Rcpp HDF5 reader/writer...")
sourceCpp("tests/interop/rw_hdf5.cpp")

message("Executing HDF5 conversion across all 28 permutations...")
process_hdf5()
message("R/hdf5lib successfully performed the dataset swap and wrote 'r_out.h5'")
