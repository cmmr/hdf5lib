# hdf5lib 2.2.0.1

* Fixed silent data corruption: ZFP with `NaN`/`NA`/`Inf` or non-native byte order, Blosc2's ZFP codecs and truncate-precision pre-filter on non-float data, and Blosc2 with array datatypes or after Fletcher32/scale-offset.
* Blosc2 `cd_values` now follow the reference HDF5-Blosc2 layout, so `h5py`/`hdf5plugin` can read and append to hdf5lib's Blosc2 datasets (including 1-D chunks). The old layout is still accepted.
* Snappy (`32003`) now writes snappy-cuda's chunk format and reads both; hdf5lib 2.2.0.0 and earlier can't read the new chunks.
* The BitRound read shim moved off SZ3's filter ID (32024) to its own (32032). Read shims no longer override real plugins.
* Blosc and Blosc2 now reuse worker threads across chunks.
* New C API: `hdf5lib_set_blosc2()`, `hdf5lib_set_blosc_nthreads()`, `hdf5lib_get_blosc_nthreads()`, Blosc2 codec/pre-filter constants, and declarations for the `H5Pset_zfp_*()` helpers.
* All filters fail cleanly on truncated or corrupt chunks.
* Datasets copied from another's creation property list (e.g. by `h5repack`) keep their ZFP, Blosc2, and Bitshuffle settings.
* Invalid settings or unsupported chunks now fail at `H5Dcreate2()`, or are stored unfiltered by optional filters, instead of failing every chunk.
* Fixed oversized-chunk overflows in Snappy and LZF, a ZFP datatype leak, and UBSan issues in the bundled ZFP.
* Compiled code no longer references `stderr` (R CMD check).
* Documentation corrections and expanded `h5py`/`hdf5plugin` interoperability tests.



# hdf5lib 2.2.0.0

* Enables installation to read-only directories.
* Dependency updates: HDF5 (2.1.1 to 2.2.0), c-blosc2 (2.23.1 to 3.3.3), and libaec (1.1.6 to 1.1.7).



# hdf5lib 2.1.1.2

* Patch `c-blosc2-2.23.1/blosc/frame.c` for `strncpy` warnings on GCC 16.



# hdf5lib 2.1.1.1

* Respect `LD` and `STRIP` env vars during linking step (allows cross-platform builds).
* Patched `strncpy` warnings introduced by GCC 16 in `src/H5FDsplitter.c`.



# hdf5lib 2.1.1.0

* Updated HDF5 to 2.1.1.
* Migrated from `zlib` to `zlib-ng` for gzip/deflate compression.
* Added Szip (`libaec`), Bzip2, LZF, LZ4, Zstandard (Zstd), Snappy, Bitshuffle, ZFP, Blosc, and Blosc2.



# hdf5lib 2.0.0.6

* Accept decimal values for `api` argument in `c_flags()` and `ld_flags()`.
* New `hdf5lib` hex logo.
* Updated documentation.



# hdf5lib 2.0.0.5

* Patched misaligned address in HDF5's `H5Tconv_enum.c` file.



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
