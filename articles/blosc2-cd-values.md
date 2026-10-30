# Blosc2 cd_values Reference

Most code should add the Blosc2 filter with `hdf5lib_set_blosc2()`,
which builds its parameters for you; see [Compression &
Filters](https://cmmr.github.io/hdf5lib/articles/compression.html#blosc2).
This page sets out those parameters, the filter’s `cd_values`, for the
rare cases that need them: calling `H5Pset_filter()` yourself, or
reading the list `H5Pget_filter_by_id()` returns for a dataset.

The plugin’s `cd_values` follow the layout of the reference HDF5-Blosc2
filter (the one `hdf5plugin`, PyTables and The HDF Group build) slot for
slot, so other implementations can read, describe and append to
`hdf5lib`’s datasets. The reference has room for a single pre-filter and
no codec parameters, so the plugin packs the rest of its settings, the
whole pre-filter pipeline and one parameter byte, into `cd_values[0]`,
which the reference only uses for its revision number and never reads.

## The layout

- **Filter ID:** `32026`
- **Elements (`cd_nelmts`):** 7

| Slot | You pass | The plugin stores |
|----|----|----|
| `cd_values[0]` | The packed settings below | The same, less anything that does not run (see below) |
| `cd_values[1]` | `0` | `0`: Blosc2 picks the block size |
| `cd_values[2]` | `0` | The item size in bytes |
| `cd_values[3]` | `0` | The chunk size in bytes |
| `cd_values[4]` | Compression level, `0` to `9` | The same, or `255` for a dataset a ZFP codec cannot take |
| `cd_values[5]` | `0`, or the code of the last pre-filter | The code of the last pre-filter |
| `cd_values[6]` | Codec | The same |
| `cd_values[7]`, `[8...]` | Nothing | The chunk rank and shape, for chunks compressed as B2ND arrays |

The packed settings in `cd_values[0]`:

| Bits  | Contents                                 |
|-------|------------------------------------------|
| 31-24 | `0xB2`, marking the value as `hdf5lib`’s |
| 23-20 | Layout version, `1`                      |
| 19-16 | Pre-filter bitmask                       |
| 15-8  | Meta value                               |
| 7-0   | `1`, the reference filter’s revision     |

That is, `cd_values[0] = 0xB2100001 + (bitmask << 16) + (meta << 8)`,
where the bitmask and meta value are `hdf5lib_set_blosc2()`’s
`prefilters` and `meta`. The bitmask is the sum of `1` (shuffle), `2`
(bitshuffle), `4` (delta) and `8` (trunc-prec), the values of
`HDF5LIB_BLOSC2_SHUFFLE` and the rest. A negative meta value goes in as
its 8-bit equivalent, `256 + meta`: `-3` is `253` (`0xFD`). [Compression
&
Filters](https://cmmr.github.io/hdf5lib/articles/compression.html#blosc2)
explains what each pre-filter and meta value does.

- **`cd_values[4]`:** Compression level (`0` to `9`). A higher level
  makes `H5Dcreate2()` fail; the plugin itself records `255` for a
  dataset a ZFP codec cannot take, so that every chunk is stored
  unfiltered.
- **`cd_values[5]`:** the last pre-filter that runs, as a single Blosc2
  filter code, which is what the reference filter keeps in this slot:
  `2` if the bitmask has bitshuffle, else `1` if shuffle, else `3` if
  delta, else `4` if trunc-prec, else `0`. The plugin works it out from
  the bitmask, so pass `0` or that code; any other value makes
  `H5Dcreate2()` fail with a message naming the code expected.
- **`cd_values[6]`:** the codec, as `hdf5lib_set_blosc2()`’s `codec`
  (`0`=blosclz, `1`=lz4, `2`=lz4hc, `4`=zlib, `5`=zstd, `32`=ndlz,
  `33`=zfp_acc, `34`=zfp_prec, `35`=zfp_rate).

The list the plugin stores records only what runs, so it may differ from
the one you passed. The ZFP codecs and NDLZ skip the pre-filters, so
their bitmask and `cd_values[5]` are stored as `0`. With bitshuffle, the
shuffle bit is dropped; truncate-precision is dropped on data it cannot
take, or when it would keep every bit. The meta value is stored as `0`
when nothing reads it, and NDLZ’s as `4` or `8`. For chunks of 2 to 16
dimensions the plugin adds the rank and shape in `cd_values[7]` onwards,
as the reference does, except for array datatypes, which it compresses
as plain Blosc2, and ZFP chunks of more than 4 dimensions or with a
dimension under 4, which it compresses as one long row. NDLZ always
records the 2-D byte image it compresses. Values you pass after
`cd_values[6]` are ignored.

## Example

Truncating floats to 16 bits of precision, applying Bitshuffle, and
compressing with LZ4 at level 5, which `hdf5lib_set_blosc2()` sets up
as:

``` c
hdf5lib_set_blosc2(plist_id, H5Z_FLAG_MANDATORY, HDF5LIB_BLOSC2_LZ4, 5,
                   HDF5LIB_BLOSC2_TRUNC_PREC | HDF5LIB_BLOSC2_BITSHUFFLE, 16);
```

The same, passed to `H5Pset_filter()` directly:

``` c
// cd_values[0]: bitmask Truncprec (8) + Bitshuffle (2) = 10, meta 16 bits -> 0xB21A1001
// cd_values[5]: Bitshuffle (2) runs last
unsigned int cd_values[7] = { 0xB21A1001, 0, 0, 0, 5, 2, 1 };
H5Pset_filter(plist_id, 32026, H5Z_FLAG_MANDATORY, 7, cd_values);
```

## Lists in other forms

The plugin reads `cd_values[0]` to tell what kind of list it was handed,
both from your `H5Pset_filter()` call and from a creation property list
copied from another dataset (as `h5repack` and `H5Dget_create_plist()`
hand them on), and converts each to the layout above:

- **`0`: the form earlier versions of `hdf5lib` documented**, still
  accepted: `{ 0, 0, 0, 0, level, bitmask, codec, meta }`, with the
  pre-filter bitmask in `cd_values[5]` and the meta value as an optional
  8th value. The example above in this form is
  `{ 0, 0, 0, 0, 5, 10, 1, 16 }`, and it stores exactly the same list.
  Here `cd_values[5]` is still the bitmask, not a filter code. The
  option tuples `hdf5plugin`’s `Blosc2()` builds also start with `0`, so
  they are read this way too, and two of its codes mean something else:
  `3` (its `DELTA`) is shuffle + bitshuffle, so bitshuffle, and `4` (its
  `TRUNC_PREC`) is delta. Passing no values at all also lands here:
  level 5, shuffle, BloscLZ.
- **`1`: the reference filter’s own lists**, such as a creation property
  list copied from a dataset `h5py` wrote. `cd_values[5]` is one filter
  code (`0` none, `1` shuffle, `2` bitshuffle, `3` delta, `4`
  trunc-prec); there is no meta value, so trunc-prec fails
  `H5Dcreate2()` on native floats (it has no precision to keep) and NDLZ
  uses 4 x 4 cells. Any other code fails `H5Dcreate2()`.
- **`2`: lists stored by `hdf5lib` 2.1.1.0 to 2.2.0.0** (and development
  builds of 2.2.0.1): the bitmask in the low byte of `cd_values[5]`, and
  the meta value in its next byte (lists of 7 values) or after the chunk
  shape.
- **`0xB2...`**: the layout above. A layout version other than `1` fails
  `H5Dcreate2()`.

Any other `cd_values[0]` fails `H5Dcreate2()`. When the plugin
compresses chunks for a dataset already in a file, such as one being
appended to, it reads the stored list the same way: a dataset an earlier
`hdf5lib` wrote keeps the settings it recorded, and one another
implementation wrote gets the single pre-filter its `cd_values[5]`
names, as the reference filter would run it. It refuses to compress with
a layout version it does not know. Reading does not depend on any of
these settings, because every Blosc2 chunk records its own pre-filters,
codec and parameters.
