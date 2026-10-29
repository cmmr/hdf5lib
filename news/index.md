# Changelog

## hdf5lib 2.2.0.1

- Blosc and Blosc2 multi-threading now pays off. The Blosc2 filter keeps
  its compression and decompression contexts, and their worker threads,
  alive between chunks instead of starting a new thread pool for every
  chunk; Blosc (v1) runs through c-blosc’s persistent global pool when
  threads are requested. Both filters still default to one thread.
- New C API `hdf5lib_set_blosc_nthreads()` /
  `hdf5lib_get_blosc_nthreads()` in `hdf5lib.h` sets the Blosc and
  Blosc2 thread count. The `BLOSC_NTHREADS` environment variable, which
  Blosc (v1) previously ignored, now applies to both filters and takes
  precedence, though the function is the better choice (see the
  compression article).
- ZFP no longer silently corrupts `NaN`, `NA`, and `Inf` values. The
  standalone ZFP filter (except in reversible mode) and Blosc2’s ZFP
  codecs now refuse chunks that hold them, so an optional filter stores
  those chunks unfiltered and a mandatory one fails the write.
- Blosc2 datasets with 1-D chunks (or more than 16 dimensions) are now
  readable by `h5py` with `hdf5plugin`. The filter used to record a
  chunk rank of 1 (or 0) in its parameters, which `hdf5plugin`’s Blosc2
  filter rejects; it now stores the reference filter’s seven values for
  such chunks. Datasets already written by hdf5lib 2.2.0.0 and earlier
  keep their old parameters: `hdf5lib` still reads them, but
  `hdf5plugin` cannot, so rewrite them to share them with Python.
- Patched the bundled ZFP to remove undefined behaviour that UBSan
  reports. Its lifting transforms now do their arithmetic in unsigned
  integers instead of relying on signed overflow and left shifts of
  negative values. Reversible mode no longer converts NaN or Inf to an
  integer when it tests whether a block can be stored exactly. Output is
  bit-for-bit unchanged.
- The ZFP filter no longer leaks a datatype ID for every dataset it is
  applied to.
- An optional ZFP filter on chunks that ZFP cannot compress now leaves
  every chunk unfiltered by design. This covers a single value, more
  than four dimensions longer than one, items other than 4 or 8 bytes,
  and data in non-native byte order. The filter used to record no
  parameters for some of these chunks and then read past them, fail
  `H5Dcreate2()` for others, and compress byte-swapped data as though it
  were native, which corrupted it. It now records parameters that make
  it refuse every chunk, and it refuses chunks whose parameters are
  missing, as in datasets earlier versions wrote this way. A mandatory
  ZFP filter still makes `H5Dcreate2()` fail on such chunks.
- ZFP’s `set_local` callback now reports errors such as an invalid mode
  instead of succeeding without recording parameters. A dataset created
  from another ZFP dataset’s creation property list, as `h5repack`
  creates them, now gets a ZFP header for its own chunks in the same
  mode. It used to inherit the original’s header, which miscompressed
  chunks of a different shape or type.
- Blosc2’s truncate-precision pre-filter (`8` in `cd_values[5]`) now
  applies only to native-order `float32` and `float64` data, and arrays
  of them; for other datatypes the flag is dropped. It used to zero the
  low bits of any 4- or 8-byte item: integers and 8-byte compound
  records were corrupted (keeping 10 bits, every `int32` below 8192 read
  back as 0), and chunks of other item sizes failed. A precision that
  Blosc2 cannot apply (keeping 0 bits, or dropping the whole mantissa)
  now makes `H5Dcreate2()` fail instead of every chunk, and keeping more
  bits than the mantissa holds keeps them all. Datasets created by
  earlier versions keep the flag, so chunks written to them later are
  still truncated.
- A Blosc2 dataset created from another’s creation property list, as
  `h5repack` creates them, now keeps the original’s meta value
  (truncation precision, or the ZFP codecs’ parameter). For chunks with
  two or more dimensions the filter took the stored chunk rank for it,
  so a 2-D copy kept 2 bits of precision, or relaxed ZFP’s accuracy to
  10^2.
- Blosc2’s ZFP codecs (`33` to `35`) no longer silently corrupt data
  that is not native-order `float32` or `float64`. They compressed
  integers and byte-swapped floats as though they were native floats,
  and the values read back as garbage (small integers as 0) without any
  error. A mandatory filter now makes `H5Dcreate2()` fail on such data.
  An optional one stores it unfiltered: it records compression level
  255, which makes every Blosc2 filter (`hdf5plugin`’s included) refuse
  each chunk. A compression level above 9 now makes `H5Dcreate2()` fail
  instead of every chunk.
- The read-only BitRound shim now sits on BitRound’s registered filter
  ID, 32032. It used to claim 32024, which belongs to SZ3. Registering
  hdf5lib’s filters then made SZ3 datasets unreadable and stopped HDF5
  from loading a real SZ3 plugin. The shim on 32023, formerly called
  BitShave, is now named for Granular BitRound, the quantizer registered
  under that ID.
- `hdf5lib_destroy_all_filters()` now also unregisters the BitGroom,
  Granular BitRound, and BitRound read shims that
  `hdf5lib_register_all_filters()` registers.
- Corrected the Blosc2 NDLZ codec ID in the compression article (`32`,
  not `11`), corrected its claims about `h5py` interoperability, and
  documented the threading and non-finite behaviour. The
  interoperability tests now cover 1-D and high-rank Blosc2 chunks,
  Blosc2’s ZFP and NDLZ codecs, and use the documented Blosc2 pre-filter
  bitmask.

## hdf5lib 2.2.0.0

CRAN release: 2026-09-09

- Enables installation to read-only directories.
- Dependency updates: HDF5 (2.1.1 to 2.2.0), c-blosc2 (2.23.1 to 3.3.3),
  and libaec (1.1.6 to 1.1.7).

## hdf5lib 2.1.1.2

CRAN release: 2026-07-21

- Patch `c-blosc2-2.23.1/blosc/frame.c` for `strncpy` warnings on GCC
  16.

## hdf5lib 2.1.1.1

CRAN release: 2026-05-15

- Respect `LD` and `STRIP` env vars during linking step (allows
  cross-platform builds).
- Patched `strncpy` warnings introduced by GCC 16 in
  `src/H5FDsplitter.c`.

## hdf5lib 2.1.1.0

CRAN release: 2026-04-02

- Updated HDF5 to 2.1.1.
- Migrated from `zlib` to `zlib-ng` for gzip/deflate compression.
- Added Szip (`libaec`), Bzip2, LZF, LZ4, Zstandard (Zstd), Snappy,
  Bitshuffle, ZFP, Blosc, and Blosc2.

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
