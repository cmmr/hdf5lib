# Compression & Filters

One of the most powerful features of HDF5 is its ability to compress
data transparently. When you read a compressed dataset, the HDF5 library
automatically decompresses it for you. This can lead to significant
savings in storage space and faster I/O performance.

`hdf5lib` provides built-in support for `gzip` and `szip` compression
natively, and additionally bundles an extensive suite of
high-performance modern filter plugins. They use the same filter IDs and
parameter layouts as Python’s `h5py` with
[`hdf5plugin`](https://github.com/silx-kit/hdf5plugin), and the
package’s interoperability tests (`tests/interop`) check both
directions: files written through `hdf5lib` read in `h5py`, and the
other way round. Two exceptions: the standalone Snappy filter (`32003`)
is not part of `hdf5plugin`, and Blosc2 datasets with 1-D chunks written
by `hdf5lib` 2.2.0.0 or earlier cannot be read by `hdf5plugin` (rewrite
them with a newer `hdf5lib`).

## Terminology

To navigate HDF5 compression effectively, it is helpful to understand
the specific terminology used throughout this guide and the broader HDF5
ecosystem:

- **Filter:** A function in the HDF5 data pipeline that transforms data
  before it is written to disk or after it is read from disk.

- **Pre-filter:** A specific type of filter (like Bitshuffle) designed
  to rearrange bytes to make the data more mathematically compressible
  before handing it off to the main compressor.

- **Compressor / Codec:** The algorithm (e.g., Zstd, LZ4, ZFP) that
  actually shrinks the physical data footprint.

- **Plugin:** An external, dynamically loaded library (e.g., a `.so` or
  `.dll` file) that provides the HDF5 library with the code needed to
  execute a filter that isn’t built into the core library.

- **Mode:** A specific operational setting for a compressor (e.g., ZFP’s
  “Fixed Accuracy” mode or LZ4’s “High Compression” mode).

## Fundamental Requirements for HDF5 Compression

Before configuring any filter, you must understand the fundamental rules
of HDF5 compression:

1.  **Chunking is Mandatory:** Compression in HDF5 requires the data to
    be stored in “chunks.” You must define a chunk size for your dataset
    using `H5Pset_chunk()`. Applying a filter to a contiguous dataset
    will fail.

2.  **Filter Registration (Plugins Only):** If you are using any of the
    bundled plugins (Zstd, LZ4, Blosc, etc.), they must be registered
    globally in your R session. You should do this exactly once from
    your package’s `R_init_<pkgname>()` entry point using
    `hdf5lib_register_all_filters()`, and tear them down in
    `R_unload_<pkgname>()` with `hdf5lib_destroy_all_filters()`.
    Registering filters per-I/O operation will severely degrade
    performance. See the [Getting
    Started](https://cmmr.github.io/hdf5lib/articles/hdf5lib.md) guide
    for the boilerplate code.

3.  **The Reader Must Have the Plugin:** If you compress a dataset using
    an external plugin (like Zstd or LZ4), **any application that opens
    that file must also have the corresponding HDF5 plugin installed.**
    If you are generating files for external users and cannot guarantee
    they have these plugins installed, use the built-in `gzip` filter
    for maximum portability.

------------------------------------------------------------------------

## Filter Implementation Details

When applying a filter via
`H5Pset_filter(plist_id, filter_id, flags, cd_nelmts, cd_values)`, you
must pass an array of `unsigned int` values known as `cd_values` (Client
Data Values). These values configure the specific behavior of the
compressor.

Below are the explicit configurations and examples for every filter
bundled with `hdf5lib`.

### 1. GZIP / Deflate

**Recommendation:** **Use for Maximum Portability.** GZIP is built
directly into the core HDF5 library, meaning files compressed with it
can be read by any standard HDF5 installation globally without needing
external plugins. However, it is significantly slower than modern
alternatives.

- **Filter ID:** `H5Z_FILTER_DEFLATE` (1)
- **Elements (`cd_nelmts`):** 1
- **`cd_values[0]`:** Compression level from `0` (none) to `9`
  (maximum).

You do not need to manually pack the `cd_values` array for GZIP. HDF5
provides a dedicated helper function.

``` c
// Apply GZIP compression with level 9
H5Pset_deflate(plist_id, 9);
```

### 2. Zstandard (Zstd)

**Recommendation:** **Use as the Default General-Purpose Compressor.**
Developed by Facebook, Zstd provides compression ratios comparable to or
better than GZIP, but at substantially faster compression and
decompression speeds.

- **Filter ID:** `32015`
- **Elements (`cd_nelmts`):** 1
- **`cd_values[0]`:** Compression level, from `1` (fastest) to `22`
  (best). Use `3` or `5` as a default; these provide an optimal balance
  of speed and file size, whereas levels above 10 offer steeply
  diminishing returns for significantly longer compression times.

``` c
unsigned int cd_values[1] = { 5 }; // Zstd level 5
H5Pset_filter(plist_id, 32015, H5Z_FLAG_MANDATORY, 1, cd_values);
```

### 3. LZ4

**Recommendation:** **Use for Maximum Speed.** LZ4 prioritizes extreme
compression and decompression speeds (often bottlenecked only by RAM
bandwidth) over finding the absolute smallest file size. Ideal for hot
data, fast pipelines, or real-time I/O.

- **Filter ID:** `32004`
- **Elements (`cd_nelmts`):** 2
- **`cd_values[0]`:** Reserved/padding (Pass `0`).
- **`cd_values[1]`:** Compression level. Passing `0` uses the standard,
  lightning-fast LZ4. Passing a value `>0` (e.g., `9`) enables LZ4-HC
  (High Compression) mode.

``` c
unsigned int cd_values[2] = { 0, 9 }; // LZ4 High Compression
H5Pset_filter(plist_id, 32004, H5Z_FLAG_MANDATORY, 2, cd_values);
```

### 4. Blosc2

**Recommendation:** **Use for Parallel Compression and Complex
Pipelines.** Blosc2 splits each chunk into blocks and can (de)compress
those blocks on several threads at once; see
[Multi-threading](#multi-threading) below. It also features a
Programmable Filter Pipeline, allowing you to easily chain multiple
pre-filters (like Delta followed by Bitshuffle) before the data hits the
final compressor.

> **Restrictions on ZFP:** When using a ZFP compressor codec (`33`,
> `34`, `35`), the data **must** be `float32` or `float64` in native
> byte order (alone or in arrays). On any other datatype, a mandatory
> filter makes `H5Dcreate2()` fail and an optional one stores the data
> unfiltered. ZFP performs lossy compression directly on numerical
> values; if bytes are shuffled before ZFP, it corrupts the dataset. To
> ensure safety, **this plugin automatically disables all Blosc2
> pre-filters if a ZFP codec is selected**. You must also ensure no
> upstream native HDF5 filters (e.g., `H5Pset_shuffle`) are applied
> before Blosc2 in your property list. ZFP also cannot store `NaN` or
> `Inf` values; see [Non-finite values](#non-finite-values) under ZFP.
> Chunks of more than 4 dimensions, or with a dimension under 4, are
> compressed as one long row, because the codec cannot take them as they
> are.

#### Plugin Configuration

`hdf5lib.h` declares a setter that adds the filter to a dataset creation
property list and builds its `cd_values` for you, along with names for
the codecs and pre-filters:

``` c
#include <hdf5lib.h>

herr_t hdf5lib_set_blosc2(hid_t plist, unsigned int flags, unsigned int codec,
                          unsigned int level, unsigned int prefilters, int meta);
```

- **`flags`:** `H5Z_FLAG_MANDATORY` or `H5Z_FLAG_OPTIONAL`, as for
  `H5Pset_filter()`. An optional filter lets the ZFP codecs store a
  chunk they cannot take unfiltered (see [Non-finite
  values](#non-finite-values) under ZFP).
- **`codec`:** `HDF5LIB_BLOSC2_BLOSCLZ` (`0`), `HDF5LIB_BLOSC2_LZ4`
  (`1`), `HDF5LIB_BLOSC2_LZ4HC` (`2`), `HDF5LIB_BLOSC2_ZLIB` (`4`),
  `HDF5LIB_BLOSC2_ZSTD` (`5`), `HDF5LIB_BLOSC2_NDLZ` (`32`),
  `HDF5LIB_BLOSC2_ZFP_ACC` (`33`), `HDF5LIB_BLOSC2_ZFP_PREC` (`34`) or
  `HDF5LIB_BLOSC2_ZFP_RATE` (`35`).
- **`level`:** the compression level, `0` to `9`.
- **`prefilters`:** the pre-filters to chain, ORed together, or `0` for
  none. They run in the order trunc-prec, delta, then bitshuffle or
  shuffle, whatever the order you list them in:
  - `HDF5LIB_BLOSC2_SHUFFLE` (`1`): Byte Shuffle
  - `HDF5LIB_BLOSC2_BITSHUFFLE` (`2`): Bitshuffle, which wins if both
    are given
  - `HDF5LIB_BLOSC2_DELTA` (`4`): Delta
  - `HDF5LIB_BLOSC2_TRUNC_PREC` (`8`): Truncate Precision, for
    native-order `float32` and `float64` data, and arrays of them, only;
    the plugin drops it for every other datatype, which it would corrupt
- **`meta`:** one more parameter, read according to the configuration:
  - *Truncate Precision:* the number of mantissa bits to keep, `1` to
    `23` for `float32` and `1` to `52` for `float64`, or a negative
    number of bits to drop instead. A value that would keep no bits or
    drop the whole mantissa makes `H5Dcreate2()` fail; one that keeps
    more bits than the mantissa holds keeps them all.
  - *NDLZ:* The cell size, `4` or `8`. Any other value is taken as `4`.
  - *ZFP:* Blosc2 maps this one value to ZFP’s parameter:
    - **Precision (`34`):** the bits of precision to keep. For 16 bits
      of precision, pass `16`.
    - **Accuracy (`33`):** a base-10 exponent: the error tolerance is
      `10^meta`. For a tolerance of `0.001`, pass `-3`.
    - **Rate (`35`):** a *percentage* of the original data size. For
      example, to achieve a rate of 8 bits per value on 32-bit floats,
      the compressed output must be exactly 25% of the original size
      (`8 / 32 = 0.25`), so you pass `25`.
  - Otherwise, pass `0`.

The setter adds the filter to the end of the pipeline, or updates the
Blosc2 filter already in it where it stands. It returns a negative
value, with the reason on HDF5’s error stack, for a level, pre-filter
set or meta value out of range. The codec, and whether the settings suit
the data, are checked by `H5Dcreate2()`.

#### Example 1: Standard Compression

Using Zstd (level 5) with the standard Bitshuffle pre-filter.

``` c
hdf5lib_set_blosc2(plist_id, H5Z_FLAG_MANDATORY, HDF5LIB_BLOSC2_ZSTD, 5, HDF5LIB_BLOSC2_BITSHUFFLE, 0);
```

#### Example 2: Multi-Filter Pipeline

Chaining the Delta filter and Bitshuffle before hitting Zstd (level 5).

``` c
hdf5lib_set_blosc2(plist_id, H5Z_FLAG_MANDATORY, HDF5LIB_BLOSC2_ZSTD, 5,
                   HDF5LIB_BLOSC2_DELTA | HDF5LIB_BLOSC2_BITSHUFFLE, 0);
```

#### Example 3: Truncate Precision Pipeline

Truncating floats to 16 bits of precision, applying Bitshuffle, and
compressing with LZ4.

``` c
hdf5lib_set_blosc2(plist_id, H5Z_FLAG_MANDATORY, HDF5LIB_BLOSC2_LZ4, 5,
                   HDF5LIB_BLOSC2_TRUNC_PREC | HDF5LIB_BLOSC2_BITSHUFFLE, 16);
```

Truncation zeroes the low bits of every value, including the payload
bits of a `NaN`. `Inf`, and a `NaN` whose payload has its top bit set
(the usual quiet `NaN`), come through unchanged, but R’s `NA_real_`
(bits `0x7FF00000000007A2`) does not. Its payload sits in the low 11
bits, so keeping 41 or fewer bits of a `float64` turns `NA` into `Inf`,
and keeping 42 to 50 bits turns it into a plain `NaN`. To keep `NA`,
leave truncation off for data that contains it.

#### Example 4: Blosc2 + ZFP Precision

Using Blosc2’s meta-compressor to run ZFP in Fixed Precision mode,
keeping exactly 16 bits of precision. ZFP skips the pre-filters, so pass
none.

``` c
hdf5lib_set_blosc2(plist_id, H5Z_FLAG_MANDATORY, HDF5LIB_BLOSC2_ZFP_PREC, 5, 0, 16);
```

#### Example 5: Blosc2 + ZFP Accuracy

Using ZFP Accuracy mode to enforce an absolute error tolerance of
`0.001` (`10^-3`).

``` c
hdf5lib_set_blosc2(plist_id, H5Z_FLAG_MANDATORY, HDF5LIB_BLOSC2_ZFP_ACC, 5, 0, -3);
```

#### Example 6: Blosc2 + ZFP Rate

Using ZFP Rate mode to compress 32-bit floats down to exactly 8 bits per
value, 25% of their size.

``` c
hdf5lib_set_blosc2(plist_id, H5Z_FLAG_MANDATORY, HDF5LIB_BLOSC2_ZFP_RATE, 5, 0, 25);
```

#### The `cd_values` layout

You need this layout only to call `H5Pset_filter()` yourself, or to read
the list `H5Pget_filter_by_id()` returns for a dataset.

The plugin’s `cd_values` follow the layout of the reference HDF5-Blosc2
filter (the one `hdf5plugin`, PyTables and The HDF Group build) slot for
slot, so other implementations can read, describe and append to
`hdf5lib`’s datasets. The reference has room for a single pre-filter and
no codec parameters, so the plugin packs the rest of its settings, the
whole pre-filter pipeline and one parameter byte, into `cd_values[0]`,
which the reference only uses for its revision number and never reads.

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
where the bitmask and meta value are the setter’s `prefilters` and
`meta`. A negative meta value goes in as its 8-bit equivalent,
`256 + meta`: `-3` is `253` (`0xFD`).

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
- **`cd_values[6]`:** the codec, as the setter’s `codec` (`0`=blosclz,
  `1`=lz4, `2`=lz4hc, `4`=zlib, `5`=zstd, `32`=ndlz, `33`=zfp_acc,
  `34`=zfp_prec, `35`=zfp_rate).

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

Example 3, passed to `H5Pset_filter()` directly:

``` c
// cd_values[0]: bitmask Truncprec (8) + Bitshuffle (2) = 10, meta 16 bits -> 0xB21A1001
// cd_values[5]: Bitshuffle (2) runs last
unsigned int cd_values[7] = { 0xB21A1001, 0, 0, 0, 5, 2, 1 };
H5Pset_filter(plist_id, 32026, H5Z_FLAG_MANDATORY, 7, cd_values);
```

#### Lists in other forms

The plugin reads `cd_values[0]` to tell what kind of list it was handed,
both from your `H5Pset_filter()` call and from a creation property list
copied from another dataset (as `h5repack` and `H5Dget_create_plist()`
hand them on), and converts each to the layout above:

- **`0`: the form earlier versions of `hdf5lib` documented**, still
  accepted: `{ 0, 0, 0, 0, level, bitmask, codec, meta }`, with the
  pre-filter bitmask in `cd_values[5]` and the meta value as an optional
  8th value. Example 3 in this form is `{ 0, 0, 0, 0, 5, 10, 1, 16 }`,
  and it stores exactly the same list as Example 3. Here `cd_values[5]`
  is still the bitmask, not a filter code. The option tuples
  `hdf5plugin`’s `Blosc2()` builds also start with `0`, so they are read
  this way too, and two of its codes mean something else: `3` (its
  `DELTA`) is shuffle + bitshuffle, so bitshuffle, and `4` (its
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

#### What `h5py` sees

- **Reading:** `h5py` with `hdf5plugin` reads every Blosc2 dataset
  `hdf5lib` writes, whatever the pre-filters and codec, as each chunk
  describes itself.
- **Describing:** `hdf5plugin.from_filter_options()` describes datasets
  that use one of the five byte codecs as
  `Blosc2(cname, clevel, filters)`, with `filters` the last pre-filter
  only, so Example 2 shows as `BITSHUFFLE`. It raises an error for NDLZ
  and the ZFP codecs.
- **Appending:** `h5py` compresses the chunks it adds with the stored
  codec and level, but runs only the pre-filter in `cd_values[5]`,
  without the meta value. Datasets whose last pre-filter is none,
  shuffle, bitshuffle or delta take new chunks without loss: Example 2’s
  new chunks are only bitshuffled, and Example 3’s are not truncated.
  Where trunc-prec runs alone, `hdf5plugin` has no precision to give it,
  so each chunk it compresses fails: a mandatory filter fails the write,
  and an optional one stores the chunk unfiltered, as `hdf5plugin`’s own
  `Blosc2(filters=TRUNC_PREC)` does. Writes to NDLZ datasets fail the
  same way. Writes to ZFP datasets either fail or, for chunks with
  recorded geometry, compress with a parameter of `0`, which loses far
  more than the dataset was written with.

#### Multi-threading

The Blosc2 and Blosc (v1) filters use **one thread** by default. To use
more, call `hdf5lib_set_blosc_nthreads()` (declared in `hdf5lib.h`) or
set the `BLOSC_NTHREADS` environment variable. As everywhere in c-blosc,
the environment variable takes precedence. Prefer the function: c-blosc2
also applies `BLOSC_NTHREADS` to the small bookkeeping contexts it
creates for every chunk, and on Windows each of those then starts and
stops threads of its own.

``` c
#include <hdf5lib.h>

hdf5lib_set_blosc_nthreads(4);           // use 4 threads from now on
int n = hdf5lib_get_blosc_nthreads();    // threads the filters will actually use
hdf5lib_set_blosc_nthreads(0);           // back to the default (BLOSC_NTHREADS, else 1)
```

Threads work on the blocks *within* a chunk, so they only help when a
chunk holds several blocks. Blosc picks the block size from the codec
and compression level. Zstd and zlib at Blosc level 9 (`cd_values[4]`),
for example, use 1 MB blocks, so a 1 MB chunk is a single block and
gains nothing from extra threads, while at level 5 the same chunk splits
into four 256 KB blocks. Larger chunks or lower levels leave more room
for parallelism. The worker threads are started on first use and kept
between chunks, so they cost nothing per chunk once running.

Keep in mind that CRAN does not allow package examples, tests, or
vignettes to use more than two threads. On Linux and macOS, a process
forked from one that has already used Blosc2 threads (a
[`parallel::mclapply()`](https://rdrr.io/r/parallel/mclapply.html)
worker, say) runs the Blosc2 filter single-threaded.

### 5. ZFP

**Recommendation:** **Use for High-Speed Lossy Compression.** ZFP
mathematically simplifies multidimensional arrays to achieve massive
compression ratios. Use when perfect bit-for-bit reconstruction is not
required.

> **Restrictions:** ZFP strictly requires numeric, multidimensional
> arrays (natively supporting 32/64-bit integers and 32/64-bit floats).
> Any filter applied to the data before it reaches ZFP will completely
> corrupt the dataset. You **must not** apply upstream HDF5 pre-filters
> (e.g., Shuffle, Bitshuffle, Scale-Offset) before ZFP in your property
> list.

ZFP also needs the datatype in native byte order, and chunks with one to
four dimensions longer than one. A single-value chunk, or a
`2 x 2 x 2 x 2 x 2` one, does not qualify. On other datasets, a
mandatory ZFP filter makes `H5Dcreate2()` fail. An optional one (see
[Non-finite values](#non-finite-values) for how to make it optional)
stays in the dataset’s filter pipeline but refuses every chunk, so HDF5
stores the data unfiltered and any reader can read it without ZFP.

*(Note: While ZFP can be invoked as a codec within the Blosc2
meta-compressor, doing so restricts it to floating-point data only and
limits the available compression modes to Accuracy, Precision, and Rate.
For integer arrays, Expert mode, or Reversible (lossless) mode, you must
use this standalone ZFP plugin).*

#### The ZFP Modes Explained Simply

- **Fixed Accuracy:** Defines a strict absolute error tolerance. Passing
  `3` ensures no decompressed value deviates from the original by more
  than $`10^{-3}`$ (`0.001`), regardless of whether the number is huge
  or tiny.

- **Fixed Precision:** Defines a sliding scale of detail. Passing `16`
  keeps roughly 5 digits of detail scaled proportionally to the size of
  the number, allowing larger absolute errors on massive numbers and
  tiny absolute errors on microscopic fractions.

- **Fixed Rate:** Defines a strict storage limit. Passing `8` forces the
  compressor to use exactly 8 bits of storage per value, guaranteeing a
  highly predictable file size.

- **Expert:** Allows manual configuration of the underlying ZFP
  parameters (`minbits`, `maxbits`, `maxprec`, `minexp`) for advanced
  users requiring highly specific encoding profiles.

- **Reversible:** Provides perfectly lossless compression, guaranteeing
  exact bit-for-bit reconstruction of the original numerical array.

#### Plugin Configuration

- **Filter ID:** `32013`
- **Elements (`cd_nelmts`):** 6
- **`cd_values[0]`:** The compression mode (`1`=Rate, `2`=Precision,
  `3`=Accuracy, `4`=Expert, `5`=Reversible/Lossless).
- **`cd_values[1-5]`:** Mode-specific parameters. For instance, in
  Precision mode (`cd_values[0]=2`), `cd_values[2]` specifies the bits
  of precision to keep, padding the rest with zeros (e.g.,
  `{2, 0, 16, 0, 0, 0}`). Rate and Accuracy take a `double`, whose 8
  bytes fill `cd_values[2]` and `cd_values[3]` in native memory order,
  as LLNL’s H5Z-ZFP and `hdf5plugin` lay it out.

While you can manually pack the 6-element `cd_values` array, `hdf5lib.h`
declares a helper for each mode that sets the parameters for you. They
keep the names that LLNL’s H5Z-ZFP plugin gives them.

``` c
#include <hdf5lib.h>

herr_t H5Pset_zfp_rate(hid_t plist, double rate);
herr_t H5Pset_zfp_precision(hid_t plist, unsigned int prec);
herr_t H5Pset_zfp_accuracy(hid_t plist, double acc);
herr_t H5Pset_zfp_expert(hid_t plist, unsigned int minbits, unsigned int maxbits,
                         unsigned int maxprec, int minexp);
herr_t H5Pset_zfp_reversible(hid_t plist);  // Lossless mode
```

``` c
// Compress data using ZFP's Accuracy mode (maintaining 0.001 tolerance)
H5Pset_zfp_accuracy(plist_id, 0.001);
```

#### Non-finite values

ZFP’s lossy modes have no encoding for `NaN` or `Inf` (or R’s `NA`,
which is a `NaN`). Left alone, ZFP would turn each one into an arbitrary
finite number and distort the other values in its block. So both the
standalone ZFP filter and Blosc2’s ZFP codecs refuse any chunk that
contains one, and what happens next depends on how the filter was added:

- **Optional** (`H5Z_FLAG_OPTIONAL`): HDF5 stores just that chunk
  unfiltered. The data stays exact and the file reads normally; every
  other chunk is still compressed.
- **Mandatory** (`H5Z_FLAG_MANDATORY`): the write fails with an HDF5
  error instead of storing corrupted values. HDF5 compresses chunks that
  fit in its chunk cache only when they are evicted or flushed, so the
  error may not appear until `H5Dclose()`. Check its return value,
  because HDF5 drops the chunk that failed.

The `H5Pset_zfp_*()` helpers add ZFP as a mandatory filter, as the
upstream H5Z-ZFP plugin does. To get the per-chunk fallback, switch it
to optional afterwards:

``` c
H5Pset_zfp_accuracy(plist_id, 0.001);
H5Pmodify_filter(plist_id, 32013, H5Z_FLAG_OPTIONAL, 0, NULL);
```

Reversible mode is lossless for every bit pattern, `NaN` and `Inf`
included, so it accepts such chunks and compresses them as usual.

### 6. Bitshuffle

**Recommendation:** **Use to Boost Compression of Structured Data.**
Bitshuffle is not a standalone compressor; it is a pre-filter that
transposes the bits in structured arrays (like integers, floats, or
compound datatypes) to expose redundancy, before handing that data off
to an internal compressor (LZ4 or Zstd). It vastly improves compression
ratios for highly structured datasets.

- **Filter ID:** `32008`
- **Elements (`cd_nelmts`):** 3
- **`cd_values[0]`:** Block size. Pass `0` to let the library choose the
  optimal default (usually 1024).
- **`cd_values[1]`:** Compressor algorithm. `0` = raw bitshuffling (no
  compression), `2` = LZ4, `3` = Zstd.
- **`cd_values[2]`:** Compression level. Only applies if Zstd is
  selected (e.g., `5`). Pass `0` for uncompressed or LZ4.

``` c
// Bitshuffle data, then apply Zstd compression at level 5
unsigned int cd_values[3] = { 0, 3, 5 }; 
H5Pset_filter(plist_id, 32008, H5Z_FLAG_MANDATORY, 3, cd_values);
```

### 7. Blosc (v1)

**Recommendation:** **Use for Backwards Compatibility.** Blosc v1 was a
pioneering meta-compressor, but it has been entirely superseded by
Blosc2 (which is faster, smarter, and supports more features). This
filter is bundled strictly to ensure backward compatibility for reading
older HDF5 files. Use Blosc2 for all new development.

- **Filter ID:** `32001`
- **Elements (`cd_nelmts`):** 7
- **`cd_values[0-3]`:** Reserved (Pass `0`).
- **`cd_values[4]`:** Compression level (`0` to `9`). *Note: Blosc
  enforces a universal 0-9 scale. If you select a codec with a wider
  range like Zstd, Blosc internally maps this 0-9 value to Zstd’s 1-22
  scale.*
- **`cd_values[5]`:** Pre-filter (`0`=nofilter, `1`=byte shuffle,
  `2`=bit shuffle).
- **`cd_values[6]`:** Compressor ID (`0`=blosclz, `1`=lz4, `2`=lz4hc,
  `3`=snappy, `4`=zlib, `5`=zstd).

``` c
// Blosc v1 using LZ4 (level 5) and the standard byte shuffle pre-filter
unsigned int cd_values[7] = { 0, 0, 0, 0, 5, 1, 1 }; 
H5Pset_filter(plist_id, 32001, H5Z_FLAG_MANDATORY, 7, cd_values);
```

### 8. SZIP

**Recommendation:** **Use for Backwards Compatibility.** Historically
used in legacy NASA and Earthdata datasets, the original SZIP algorithm
carried strict licensing encumbrances that stifled widespread adoption.
Modern tools like ZFP or Blosc2 are vastly superior for scientific data.
This is included primarily for reading legacy archives (note: `hdf5lib`
safely circumvents historical licensing issues by bundling `libaec`, a
permissively licensed, drop-in replacement).

> **Restrictions:** SZIP strictly requires numeric data (integers or
> floating-point numbers). It does not support compound datatypes,
> strings, or variable-length arrays. The chunk size (in elements) must
> also be an exact multiple of the block size.

- **Filter ID:** `H5Z_FILTER_SZIP` (4)
- **Elements (`cd_nelmts`):** 2
- **`cd_values[0]`:** Options mask (e.g., `H5_SZIP_NN_OPTION_MASK` or
  `H5_SZIP_EC_OPTION_MASK`).
- **`cd_values[1]`:** Pixels per block. Must be even, typically `8`,
  `16`, or `32`.

Like GZIP, SZIP has a dedicated helper function.

``` c
// Apply SZIP compression (Entropy Coding method, 8 pixels per block)
H5Pset_szip(plist_id, H5_SZIP_EC_OPTION_MASK, 8);
```

### 9. BZIP2

**Recommendation:** **Use for Backwards Compatibility.** While BZIP2
historically offered fantastic compression ratios, it is notoriously
slow to compress and decompress. Zstd now achieves comparable or better
ratios in a fraction of the time. Bundled strictly to allow reading of
legacy archives.

- **Filter ID:** `307`
- **Elements (`cd_nelmts`):** 1
- **`cd_values[0]`:** Block size / Compression level from `1` (fastest)
  to `9` (best). Default is `9`.

``` c
unsigned int cd_values[1] = { 9 }; // BZIP2 level 9
H5Pset_filter(plist_id, 307, H5Z_FLAG_MANDATORY, 1, cd_values);
```

### 10. LZF & Snappy

**Recommendation:** **Use for Backwards Compatibility.** Both LZF and
Snappy were early pioneers of extreme-speed compression. However, LZ4
generally outperforms them in modern environments. LZF is included
primarily because it was the default high-speed compressor in early
versions of the Python `h5py` library.

Both LZF and Snappy are extremely fast, low-overhead algorithms. They
require zero configuration parameters.

- **LZF Filter ID:** `32000`
- **Snappy Filter ID:** `32003`
- **Elements (`cd_nelmts`):** 0

``` c
// Apply Snappy compression
H5Pset_filter(plist_id, 32003, H5Z_FLAG_MANDATORY, 0, NULL);
```

> **Snappy’s chunk layout:** HDF5’s filter registry assigns `32003` to
> [snappy-cuda](https://github.com/lucasvr/snappy-cuda), so `hdf5lib`
> writes chunks in snappy-cuda’s layout, and the two read each other’s
> files. A chunk holds the uncompressed length and a block size of 32
> KiB, both as varints, then each block as a 4-byte little-endian length
> followed by that block’s raw Snappy data. Every block but the last is
> full, as snappy-cuda’s GPU decoder requires. `hdf5lib` writes it with
> the bundled csnappy library, so no CUDA or GPU is needed. `hdf5lib`
> reads chunks in this layout with any block size, and still reads the
> single raw Snappy stream per chunk that `hdf5lib` 2.2.0.0 and earlier
> wrote. **Those older releases cannot read Snappy chunks written by
> `hdf5lib` 2.2.0.1 or later.**
>
> `h5py` with `hdf5plugin` has no `32003` filter. Blosc’s `snappy` codec
> (filter `32001` with `cd_values[6] = 3`) is unrelated: it wraps Snappy
> in Blosc’s own container, and it is the way to share Snappy-compressed
> data with Python.

------------------------------------------------------------------------

## Dynamically Loading Additional External Filters

While `hdf5lib` bundles an extensive suite of filters, it cannot include
every possible algorithm. For instance, high-performance filters like
**SZ3** or **VBZ** are not bundled in order to maintain the package’s
strict C-only, zero-dependency footprint.

However, HDF5 supports dynamically loading these algorithms at runtime
through its plugin mechanism. `hdf5lib` is fully compiled with the
necessary flags to enable this feature.

### How It Works for the User

1.  **Install the Filter:** The user obtains and installs the desired
    filter plugin (`.so`, `.dylib`, or `.dll` files).
2.  **Set the Plugin Path:** The user must tell the HDF5 library where
    to find the installed plugins by setting the `HDF5_PLUGIN_PATH`
    environment variable.

Once configured, any R package linking to `hdf5lib` can seamlessly read
datasets using that external filter.

``` r

# Tell HDF5 where to find external filter plugins
Sys.setenv(HDF5_PLUGIN_PATH = "/opt/hdf5/plugins/")

# Now, any function that uses hdf5lib can decompress an SZ3 file automatically
```
