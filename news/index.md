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
- New C API `hdf5lib_set_blosc2()` in `hdf5lib.h` adds the Blosc2 filter
  to a dataset creation property list from a codec, compression level,
  pre-filters and meta value, so callers no longer work out the packed
  `cd_values[0]` by hand. `hdf5lib.h` also names the codecs and
  pre-filters (`HDF5LIB_BLOSC2_ZSTD`, `HDF5LIB_BLOSC2_BITSHUFFLE` and so
  on), and a negative meta value, such as ZFP accuracy’s exponent, is
  passed as it is.
- `hdf5lib.h` now declares the ZFP helpers `H5Pset_zfp_rate()`,
  `H5Pset_zfp_precision()`, `H5Pset_zfp_accuracy()`,
  `H5Pset_zfp_expert()` and `H5Pset_zfp_reversible()`, so callers no
  longer need to write `extern` declarations for them. Existing `extern`
  declarations still compile.
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
- Blosc2 (`32026`) has a new `cd_values` layout, in which every slot
  holds what the reference HDF5-Blosc2 filter keeps there.
  `cd_values[5]` is now the last pre-filter as a single Blosc2 filter
  code (`0` none, `1` shuffle, `2` bitshuffle, `3` delta, `4`
  trunc-prec), and nothing follows the chunk shape. hdf5lib’s own
  settings, the pre-filter bitmask and the meta value (trunc-prec’s
  precision, NDLZ’s cell size or the ZFP codecs’ parameter), are packed
  into `cd_values[0]`, where the reference keeps a revision number it
  never reads: `0xB2100001 + (bitmask << 16) + (meta << 8)`.
  `hdf5plugin` read the old bitmask as a filter code, so it took delta
  (`4`) for trunc-prec and rejected pipelines such as delta + bitshuffle
  (`6`); it now describes hdf5lib’s datasets by their last pre-filter,
  and `h5py` can append to them. Pass the new form, 7 values, to
  `H5Pset_filter()`. The old form, with `cd_values[0] = 0`, the bitmask
  in `cd_values[5]` and the meta value as an 8th value, is still
  accepted and stored in the new layout. Creation property lists copied
  from datasets that earlier versions or the reference filter wrote are
  converted too, and chunks appended to those datasets keep their
  settings. hdf5lib used to read a reference dataset’s `cd_values[5]` as
  a bitmask when appending to it, running delta (`3`) as bitshuffle. See
  the compression article for the layout and for what `h5py` does with
  each kind of dataset.
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
- Blosc2’s truncate-precision pre-filter (bitmask `8`) now applies only
  to native-order `float32` and `float64` data, and arrays of them; for
  other datatypes the flag is dropped. It used to zero the low bits of
  any 4- or 8-byte item: integers and 8-byte compound records were
  corrupted (keeping 10 bits, every `int32` below 8192 read back as 0),
  and chunks of other item sizes failed. A precision that Blosc2 cannot
  apply (keeping 0 bits, or dropping the whole mantissa) now makes
  `H5Dcreate2()` fail instead of every chunk, and keeping more bits than
  the mantissa holds keeps them all. Datasets created by earlier
  versions keep the flag, so chunks written to them later are still
  truncated.
- A Blosc2 dataset created from another’s creation property list, as
  `h5repack` creates them, now keeps the original’s meta value
  (truncation precision, or the ZFP codecs’ parameter). For chunks with
  two or more dimensions the filter took the stored chunk rank for it,
  so a 2-D copy kept 2 bits of precision, or relaxed ZFP’s accuracy to
  10^2. The meta value is now stored in `cd_values[0]`, away from the
  chunk shape.
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
- Blosc2 no longer loses data when a chunk’s bytes don’t match the chunk
  geometry it records. Three cases hit this: an array datatype with 2-D
  or larger chunks, NDLZ with an array datatype, and a filter that
  changes a chunk’s size (Fletcher32 or scale-offset) running before
  Blosc2. Only part of each chunk was compressed, so writes succeeded
  and every read failed. Array datatypes are now recorded without B2ND
  geometry, and a chunk whose size doesn’t match its geometry falls back
  to plain Blosc2, as the reference filter does.
- Blosc2’s ZFP codecs no longer fail every chunk with a rank above 4 or
  a dimension under 4, shapes that c-blosc2’s ZFP codec rejects. Such
  chunks now use 1-D ZFP. A chunk of fewer than 4 values is handled like
  a datatype ZFP can’t take. Blosc2 now also reads datasets whose filter
  parameters hold fewer than 7 values, as the reference filter writes
  them for 1-D chunks.
- Every filter now fails cleanly on a truncated or corrupt chunk.
  Previously bzip2 hung, Snappy returned an uninitialized tail, LZ4
  didn’t check each block’s decoded length, and Blosc, Bitshuffle and
  ZFP could read past the end of the chunk.
- The Snappy filter (`32003`) now writes chunks in the layout of
  [snappy-cuda](https://github.com/lucasvr/snappy-cuda), the filter that
  HDF5’s registry assigns that ID to, so the two can read each other’s
  files. A chunk holds the uncompressed length and a 32 KiB block size
  as varints, then each block as a 4-byte little-endian length and its
  raw Snappy data. The filter used to write one raw Snappy stream per
  chunk, which snappy-cuda can’t read. It reads both layouts,
  snappy-cuda’s at any block size, and still uses only the bundled
  csnappy, so no CUDA is needed. `hdf5lib` 2.2.0.0 and earlier can’t
  read Snappy chunks written by this version. Blosc’s `snappy` codec is
  a different container (filter `32001`) and remains the way to share
  Snappy-compressed data with `h5py` and `hdf5plugin`, which have no
  `32003` filter.
- Snappy now refuses chunks of about 3.4 GB or more, whose worst-case
  compressed size overflowed its output buffer. LZF refuses chunks of 4
  GB or more, which it used to truncate silently; HDF5 2.x allows chunks
  that large.
- ZFP now byte-swaps data before compressing chunks appended to a
  dataset written on a machine of the other byte order, matching how
  such chunks are read. It refuses a chunk whose size differs from what
  its ZFP header describes, where it used to read past the chunk. Chunk
  dimensions too large for ZFP’s header are now treated like other
  chunks ZFP can’t take, so an optional filter declines them instead of
  failing `H5Dcreate2()`. A dataset created from a declined dataset’s
  creation property list is declined again instead of failing.
- Bitshuffle: a dataset created from another Bitshuffle dataset’s
  creation property list, as `h5repack` creates them, now keeps its
  codec. `set_local` used to shift the copied parameters again, so every
  chunk failed. A block size that isn’t a multiple of 8, or an unknown
  codec, now makes `H5Dcreate2()` fail instead of every chunk. An
  element size of 0 no longer divides by zero. Unknown codec IDs in
  existing files are read as plain bitshuffle, as the reference filter
  reads them.
- The read-only BitGroom, Granular BitRound and BitRound shims are now
  registered only for IDs no other filter has, such as a real plugin
  found through `HDF5_PLUGIN_PATH`, which can also write.
  `hdf5lib_destroy_all_filters()` unregisters only the shims it
  registered.
- Compiled code no longer refers to the C library’s `stderr`, which R
  CMD check flags in packages that link hdf5lib. The Blosc filter now
  reports errors only through HDF5’s error stack, and bzip2 is built
  with `BZ_NO_STDIO`. c-blosc2’s `BLOSC_TRACE` and `BLOSC_INFO` messages
  are compiled out, because they could run on Blosc worker threads,
  where R’s console can’t be used.
- The smoke tests now read data back through a fresh dataset handle, so
  every chunk is decompressed rather than served from HDF5’s chunk
  cache.
- Corrected the Blosc2 NDLZ codec ID in the compression article (`32`,
  not `11`), corrected its claims about `h5py` interoperability, and
  documented the threading and non-finite behaviour. The
  interoperability tests now cover 1-D and high-rank Blosc2 chunks,
  Blosc2’s ZFP and NDLZ codecs, and the old and new Blosc2 `cd_values`
  forms, and check that `hdf5plugin` describes hdf5lib’s Blosc2 datasets
  and that `h5py` can append to them.

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
