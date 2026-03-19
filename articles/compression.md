# Compression & Filters

One of the most powerful features of HDF5 is its ability to compress
data transparently. When you read a compressed dataset, the HDF5 library
automatically decompresses it for you. This can lead to significant
savings in storage space and faster I/O performance.

`hdf5lib` provides built-in support for `gzip` and `szip` compression
natively, and additionally bundles an extensive suite of
high-performance modern filter plugins.

## The Golden Rules of HDF5 Compression

Before configuring any filter, you must understand the two fundamental
requirements of HDF5 compression:

1.  **Chunking is Mandatory:** Compression in HDF5 requires the data to
    be stored in “chunks.” You must define a chunk size for your dataset
    using `H5Pset_chunk()`. Applying a filter to a contiguous dataset
    will fail.

2.  **Filter Registration (Plugins Only):** If you are using any of the
    bundled plugins (Zstd, LZ4, Blosc, etc.), they must be registered
    globally in your R session. You should do this exactly once during
    your package’s `.onLoad` hook using `hdf5lib_register_all_filters()`
    and tear them down in `.onUnload` with
    `hdf5lib_destroy_all_filters()`. Registering filters per-I/O
    operation will severely degrade performance. See the [Getting
    Started](https://cmmr.github.io/hdf5lib/articles/hdf5lib.md) guide
    for the boilerplate code.

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

### 2. SZIP

- **Filter ID:** `H5Z_FILTER_SZIP` (4)
- **Elements (`cd_nelmts`):** 2
- **`cd_values[0]`:** Options mask (e.g., `H5_SZIP_NN_OPTION_MASK` or
  `H5_SZIP_EC_OPTION_MASK`).
- **`cd_values[1]`:** Pixels per block. Must be even, typically `8`,
  `16`, or `32`.

Like GZIP, SZIP has a dedicated helper function.

> **Restriction:** SZIP strictly requires numeric data. It does not
> support strings or variable-length types. The chunk size (in elements)
> must also be an exact multiple of the block size.

``` c
// Apply SZIP compression (Entropy Coding method, 8 pixels per block)
H5Pset_szip(plist_id, H5_SZIP_EC_OPTION_MASK, 8);
```

### 3. Zstandard (Zstd)

- **Filter ID:** `32015`
- **Elements (`cd_nelmts`):** 1
- **`cd_values[0]`:** Compression level, generally from `1` (fastest) to
  `22` (best). A highly recommended default is `3` or `5`.

``` c
unsigned int cd_values[1] = { 5 }; // Zstd level 5
H5Pset_filter(plist_id, 32015, H5Z_FLAG_MANDATORY, 1, cd_values);
```

### 4. LZ4

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

### 5. Bitshuffle

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

### 6. Blosc (v1)

The original Blosc meta-compressor. While Blosc2 is the modern standard,
Blosc v1 is fully supported for backwards compatibility.

- **Filter ID:** `32001`
- **Elements (`cd_nelmts`):** 7
- **`cd_values[0-3]`:** Reserved (Pass `0`).
- **`cd_values[4]`:** Compression level (`0` to `9`).
- **`cd_values[5]`:** Pre-filter (`0`=nofilter, `1`=byte shuffle,
  `2`=bit shuffle).
- **`cd_values[6]`:** Compressor ID (`0`=blosclz, `1`=lz4, `2`=lz4hc,
  `3`=snappy, `4`=zlib, `5`=zstd).

``` c
// Blosc v1 using LZ4 (level 5) and the standard byte shuffle pre-filter
unsigned int cd_values[7] = { 0, 0, 0, 0, 5, 1, 1 }; 
H5Pset_filter(plist_id, 32001, H5Z_FLAG_MANDATORY, 7, cd_values);
```

### 7. Blosc2

Blosc2 is a high-performance meta-compressor capable of utilizing
internal thread pools to compress blocks of data in parallel. Data
written via the Blosc2 plugin in `hdf5lib` is fully interoperable with
the Python `h5py` ecosystem.

A major upgrade in Blosc2 is the **Programmable Filter Pipeline**, which
allows you to chain up to 6 distinct pre-filters (e.g., Delta followed
by Bitshuffle) before the data hits the final compressor. Our plugin
implementation is uniquely engineered to pack these complex pipelines
into HDF5 safely, ensuring the resulting dataset remains 100% decodable
by standard `h5py` and community plugins.

> **Warning on ZFP and Pre-filters:** If you choose ZFP as the internal
> Blosc2 codec (`cd_values[6]=6`), you **must** set the pre-filter to
> `0` (nofilter). Pre-filters like Shuffle or Bitshuffle rearrange the
> byte structure of your data. ZFP will then interpret this shuffled
> byte stream as actual numerical values and apply lossy encoding to it.
> When these lossily-altered bits are later unshuffled, it results in
> catastrophic bit-level corruption, introducing what amounts to random
> bit flips in your original numbers.

#### Standard (Single Filter) Format

If you only need one pre-filter, use the standard 8-element
configuration:

- **Filter ID:** `32026`
- **Elements (`cd_nelmts`):** 8
- **`cd_values[0-3]`:** Reserved (Pass `0`).
- **`cd_values[4]`:** Compression level (`0` to `9`).
- **`cd_values[5]`:** Pre-filter (`0`=nofilter, `1`=shuffle,
  `2`=bitshuffle, `3`=delta, `4`=truncprec).
- **`cd_values[6]`:** Compressor ID (`0`=blosclz, `1`=lz4, `2`=lz4hc,
  `3`=snappy, `4`=zlib, `5`=zstd, `6`=zfp, `11`=ndlz).
- **`cd_values[7]`:** Metadata. Usually `0`. If using the `truncprec`
  filter (`cd_values[5]=4`), this defines the number of bits of
  precision to keep (e.g., `16`).

``` c
// Blosc2 using Zstd (level 5) and the Bitshuffle pre-filter
unsigned int cd_values[8] = { 0, 0, 0, 0, 5, 2, 5, 0 }; 
H5Pset_filter(plist_id, 32026, H5Z_FLAG_MANDATORY, 8, cd_values);
```

#### Multi-Filter Pipeline Format

To chain multiple pre-filters, expand the `cd_values` array. The plugin
will automatically parse this array and dynamically translate it during
dataset creation to preserve strict `h5py` layout compatibility.

- **Elements (`cd_nelmts`):** `8 + (2 * N)`, where `N` is the number of
  filters.
- **`cd_values[0-3]`:** Reserved (Pass `0`).
- **`cd_values[4]`:** Compression level (`0` to `9`).
- **`cd_values[5]`:** Legacy fallback pre-filter (Typically matches your
  last filter, e.g., `2` for bitshuffle. Required for legacy fallback).
- **`cd_values[6]`:** Compressor ID.
- **`cd_values[7]`:** Number of pipeline filters `N` (maximum of 6).
- **`cd_values[8 ... 8+N-1]`:** The sequence of Filter IDs (e.g., `3`
  for Delta, then `2` for Bitshuffle).
- **`cd_values[8+N ... 8+2N-1]`:** Metadata for each respective filter
  (usually `0`).

``` c
// Blosc2 Pipeline: Delta -> Bitshuffle -> Zstd (level 5)
unsigned int cd_pipeline[12] = {0};
cd_pipeline[4] = 5; // Compression level
cd_pipeline[5] = 2; // Legacy fallback (Bitshuffle)
cd_pipeline[6] = 5; // Compressor (Zstd)

cd_pipeline[7] = 2; // Number of pipeline filters

// Filter IDs
cd_pipeline[8] = 3; // Filter 1: Delta
cd_pipeline[9] = 2; // Filter 2: Bitshuffle

// Filter Metadata
cd_pipeline[10] = 0; // Meta for Delta
cd_pipeline[11] = 0; // Meta for Bitshuffle

H5Pset_filter(plist_id, 32026, H5Z_FLAG_MANDATORY, 12, cd_pipeline);
```

### 8. ZFP

ZFP is designed for high-speed, lossy (and lossless) compression of
floating-point and integer arrays.

- **Filter ID:** `32013`
- **Elements (`cd_nelmts`):** 6
- **`cd_values[0]`:** The compression mode (`1`=Rate, `2`=Precision,
  `3`=Accuracy, `4`=Expert, `5`=Reversible/Lossless).
- **`cd_values[1-5]`:** Mode-specific parameters. For instance, in
  Precision mode (`cd_values[0]=2`), `cd_values[2]` specifies the bits
  of precision to keep, padding the rest with zeros (e.g.,
  `{2, 0, 16, 0, 0, 0}`).

> **Restriction:** ZFP strictly requires numeric, multidimensional
> arrays. It will fail if applied to strings, compound datatypes, or 1D
> arrays of bytes.
>
> **Warning on Pre-filters:** If you are using ZFP in a lossy mode
> (Rate, Precision, or Accuracy) via a filter pipeline, do **not** apply
> pre-filters like Shuffle or Bitshuffle before it. Because shuffling
> rearranges the byte structure, ZFP ends up interpreting and lossily
> encoding the rearranged byte stream as if they were the actual
> numerical values. Unshuffling these lossily-modified bits during
> decompression will result in catastrophic, random bit flips in your
> final data.

While you can manually pack the 6-element `cd_values` array, the ZFP
plugin exposes convenient external helper functions that you can declare
in your C code to set the parameters effortlessly.

``` c
// Declare the external helpers provided by the bundled ZFP plugin
extern herr_t H5Pset_zfp_rate(hid_t plist, double rate);
extern herr_t H5Pset_zfp_precision(hid_t plist, unsigned int prec);
extern herr_t H5Pset_zfp_accuracy(hid_t plist, double acc);
extern herr_t H5Pset_zfp_reversible(hid_t plist); // Lossless mode

// ... later in your code ...

// Compress data using ZFP's Accuracy mode (maintaining 0.001 tolerance)
H5Pset_zfp_accuracy(plist_id, 0.001);
```

### 9. BZIP2

- **Filter ID:** `307`
- **Elements (`cd_nelmts`):** 1
- **`cd_values[0]`:** Block size / Compression level from `1` (fastest)
  to `9` (best). Default is `9`.

``` c
unsigned int cd_values[1] = { 9 }; // BZIP2 level 9
H5Pset_filter(plist_id, 307, H5Z_FLAG_MANDATORY, 1, cd_values);
```

### 10. LZF & Snappy

Both LZF and Snappy are extremely fast, low-overhead algorithms. They
require zero configuration parameters.

- **LZF Filter ID:** `32000`
- **Snappy Filter ID:** `32003`
- **Elements (`cd_nelmts`):** 0

``` c
// Apply Snappy compression
H5Pset_filter(plist_id, 32003, H5Z_FLAG_MANDATORY, 0, NULL);
```

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
