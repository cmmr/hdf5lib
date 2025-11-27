# Changelog

## hdf5lib 2.0.0.2

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
