# Changelog

## hdf5lib 2.0.0.7

- Added SZIP compression support by bundling `libaec`, avoiding the
  licensing restrictions of the original SZIP library.

## hdf5lib 2.0.0.6

CRAN release: 2026-02-19

- Accept decimal values for `api` argument in
  [`c_flags()`](https://cmmr.github.io/hdf5lib/reference/c_flags.md) and
  [`ld_flags()`](https://cmmr.github.io/hdf5lib/reference/ld_flags.md).
- New `hdf5lib` hex logo.
- Updated documentation.

## hdf5lib 2.0.0.5

CRAN release: 2026-01-14

- Patched misaligned address in HDF5’s `H5Tconv_enum.c` file.

## hdf5lib 2.0.0.4

CRAN release: 2025-12-22

- Patched zlib `win32/Makefile.gcc` for conda-forge compatibility.

## hdf5lib 2.0.0.3

CRAN release: 2025-12-14

- Patched misaligned address in HDF5’s `H5Tvlen.c` file.
- Fix for Alpine Linux (`musl`) CRAN check.
- Clarified that
  [`c_flags()`](https://cmmr.github.io/hdf5lib/reference/c_flags.md) and
  `ld_flags`’s `api` argument should be numeric.

## hdf5lib 2.0.0.2

CRAN release: 2025-12-02

- Updated to HDF5 2.0.0
- Added `api` argument to `c_flags` to control exposed HDF5 API.
- Customized HDF5 build configuration for R environment.

## hdf5lib 1.14.6.9

- Added additional `CPPFLAGS` include directories needed by Fedora
  builders.
- Automated build testing using `rhub` github actions.

## hdf5lib 1.14.6.8

CRAN release: 2025-11-19

- Fixed compiler flags for Fedora builders.
- Renamed `libhdf5.a` to `libhdf5z.a` eliminate ambiguity with system
  libraries.
- Paths returned by
  [`c_flags()`](https://cmmr.github.io/hdf5lib/reference/c_flags.md) and
  [`ld_flags()`](https://cmmr.github.io/hdf5lib/reference/ld_flags.md)
  are only quoted when necessary.

## hdf5lib 1.14.6.7

CRAN release: 2025-11-17

- Corrected link to `inst/COPYRIGHTS`.

## hdf5lib 1.14.6.6

- Added HDF5 and zlib copyright holders to DESCRIPTION.

## hdf5lib 1.14.6.5

- Fixed compilation error identified on CRAN’s Fedora builders.

## hdf5lib 1.14.6.4

CRAN release: 2025-11-10

- Initial CRAN submission.
