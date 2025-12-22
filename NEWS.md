# hdf5lib 2.0.0.4

* Patched zlib `win32/Makefile.gcc` for conda-forge compatibility.



# hdf5lib 2.0.0.3

* Patched misaligned address in HDF5's `H5Tvlen.c` file.
* Fix for Alpine Linux (`musl`) CRAN check.
* Clarified that `c_flags()` and `ld_flags`'s `api` argument should be numeric.



# hdf5lib 2.0.0.2

* Updated to HDF5 2.0.0
* Added `api` argument to `c_flags` to control exposed HDF5 API.
* Customized HDF5 build configuration for R environment.



# hdf5lib 1.14.6.9

* Added additional `CPPFLAGS` include directories needed by Fedora builders.
* Automated build testing using `rhub` github actions.



# hdf5lib 1.14.6.8

* Fixed compiler flags for Fedora builders.
* Renamed `libhdf5.a` to `libhdf5z.a` eliminate ambiguity with system libraries.
* Paths returned by `c_flags()` and `ld_flags()` are only quoted when necessary.



# hdf5lib 1.14.6.7

* Corrected link to `inst/COPYRIGHTS`.



# hdf5lib 1.14.6.6

* Added HDF5 and zlib copyright holders to DESCRIPTION.



# hdf5lib 1.14.6.5

* Fixed compilation error identified on CRAN's Fedora builders.



# hdf5lib 1.14.6.4

* Initial CRAN submission.
