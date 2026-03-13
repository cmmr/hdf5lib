# Compression & Filters

One of the most powerful features of HDF5 is its ability to compress
data transparently. When you read a compressed dataset, the HDF5 library
automatically decompresses it for you. This can lead to significant
savings in storage space and faster I/O performance by reducing the
amount of data that needs to be transferred from disk.

`hdf5lib` provides built-in support for `gzip` and `szip` compression
natively, and additionally bundles a full suite of high-performance
modern filter plugins. It also enables the HDF5 library’s dynamic filter
mechanism, allowing users to leverage an even wider range of external
compression algorithms at runtime.

## Native Compression (gzip and szip)

`hdf5lib` bundles the `zlib` and `libaec` libraries, which provide the
`deflate` (commonly known as `gzip`) and `szip` compression algorithms
directly to the core HDF5 library. This means any package linking to
`hdf5lib` can create and read these datasets out-of-the-box, with no
extra configuration or plugin registration.

To create a compressed dataset, you must do two things:

1.  **Enable Chunking:** Compression in HDF5 requires the data to be
    stored in “chunks.” You must define a chunk size for your dataset.
2.  **Set the Filter:** You must add the `deflate` (or other) filter to
    the dataset creation property list.

### C++ Example

The following `Rcpp` example demonstrates how to create a chunked and
`gzip`-compressed dataset.

``` cpp
#include <Rcpp.h>
#include <hdf5.h>
#include <vector>

//' Create a compressed dataset
//'
//' @param filename Path to the HDF5 file.
//' @param dsetname Name of the dataset to create.
//' @export
// [[Rcpp::export]]
void create_compressed_dataset(std::string filename, std::string dsetname) {
    // Some sample data
    std::vector<int> data(1000, 42);
    hsize_t dims = { data.size() };

    // 1. Create the file and dataspace
    hid_t file_id = H5Fcreate(filename.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
    hid_t space_id = H5Screate_simple(1, dims, NULL);

    // 2. Create a dataset creation property list
    hid_t dcpl_id = H5Pcreate(H5P_DATASET_CREATE);

    // 3. Enable chunking (required for compression)
    hsize_t chunk_dims = { 100 };
    H5Pset_chunk(dcpl_id, 1, chunk_dims);

    // 4. Set the deflate (gzip) filter with compression level 6
    H5Pset_deflate(dcpl_id, 6);

    // 5. Create the dataset using the property list
    hid_t dset_id = H5Dcreate2(file_id, dsetname.c_str(), H5T_NATIVE_INT,
                                space_id, H5P_DEFAULT, dcpl_id, H5P_DEFAULT);

    // 6. Write the data
    H5Dwrite(dset_id, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, data.data());

    // 7. Clean up all resources
    H5Pclose(dcpl_id);
    H5Dclose(dset_id);
    H5Sclose(space_id);
    H5Fclose(file_id);
}
```

When you read this dataset back (using any HDF5-aware tool), the
decompression will be handled automatically.

## Bundled Filter Plugins

In addition to native GZIP and SZIP, `hdf5lib` bundles several
high-performance filter plugins.

### Initializing the Plugins (Best Practice)

To use the non-native plugins (everything except GZIP and SZIP), you
must explicitly register them using the functions provided in
`hdf5lib.h`.

**Crucial Warning on Performance:** When you register filters using
`hdf5lib_register_all_filters()`, it modifies the global state of the
HDF5 library and initializes the Blosc2 engine, which spins up a
background pool of worker threads for parallel compression. Tearing this
down via `hdf5lib_destroy_all_filters()` joins and destroys those
threads.

You should **never** register and destroy filters per-I/O operation.
Doing so will severely impact performance through thread thrashing and
locking overhead. Instead, it is highly recommended to tie registration
to your R package’s lifecycle using `.onLoad` and `.onUnload` hooks.

**1. Create C Wrappers (e.g., `src/init.c`)**

``` c
#include <Rinternals.h>
#include "hdf5lib.h"

SEXP r_register_hdf5_filters() {
    hdf5lib_register_all_filters();
    return R_NilValue;
}

SEXP r_destroy_hdf5_filters() {
    hdf5lib_destroy_all_filters();
    return R_NilValue;
}
```

**2. Set up R Hooks (e.g., `R/zzz.R`)**

``` r
.onLoad <- function(libname, pkgname) {
    # Initialize plugins and thread pools once when the package loads
    .Call("r_register_hdf5_filters", PACKAGE = pkgname)
}

.onUnload <- function(libpath) {
    # Tear down thread pools cleanly on exit
    .Call("r_destroy_hdf5_filters", PACKAGE = "yourpackage")
}
```

### Available Filters Summary

The following table summarizes the compression filters available
natively or via bundled plugins in `hdf5lib`.

| Filter Name          | HDF5 Filter ID    | Target Use Case                                                            | Data Restrictions                               |
|:---------------------|:------------------|:---------------------------------------------------------------------------|:------------------------------------------------|
| **GZIP (Deflate)**   | `1` (Native)      | General purpose, universal compatibility.                                  | None                                            |
| **SZIP**             | `4` (Native)      | Legacy scientific data compression.                                        | **Numeric data only.** Strict block size rules. |
| **BZIP2**            | `307`             | General purpose, high compression ratio.                                   | None                                            |
| **LZF**              | `32000`           | Very fast, lightweight compression.                                        | None                                            |
| **Blosc / Blosc2**   | `32001` / `32026` | Meta-compressor. Extremely fast, multi-threaded capable block compression. | None                                            |
| **Snappy**           | `32003`           | Fast, low-CPU overhead compression (Google).                               | None                                            |
| **LZ4**              | `32004`           | Lightning fast decompression.                                              | None                                            |
| **Bitshuffle**       | `32008`           | Reorders bits for highly compressible layouts.                             | None                                            |
| **ZFP**              | `32013`           | Lossy/lossless compression for multidimensional arrays.                    | **Numeric arrays only** (Float/Int).            |
| **Zstandard (Zstd)** | `32015`           | Modern standard balancing extreme speed and high compression.              | None                                            |

### Filter Details and `cd_values`

When applying a filter via
`H5Pset_filter(plist_id, filter_id, flags, cd_nelmts, cd_values)`, you
must pass an array of `unsigned int` values known as `cd_values` (Client
Data Values). These configure how the filter behaves (e.g., compression
level).

Below are the expected parameters for each bundled filter.

**GZIP / Deflate (`H5Z_FILTER_DEFLATE`)**

Instead of `H5Pset_filter`, you can apply this conveniently using
`H5Pset_deflate(plist_id, level)`.

- **Elements (`cd_nelmts`)**: 1
- **`cd_values[0]`**: Compression level from `0` (no compression) to `9`
  (maximum compression). Default is usually `6`.

**SZIP (`H5Z_FILTER_SZIP`)**

You can apply it using
`H5Pset_szip(plist_id, options_mask, pixels_per_block)`.

- **Restriction:** **Numeric data only.** Does not support strings or
  variable length types. The chunk size (in elements) must be an exact
  multiple of the block size.
- **Elements (`cd_nelmts`)**: 2
- **`cd_values[0]`**: Options mask (e.g., `H5_SZIP_NN_OPTION_MASK` or
  `H5_SZIP_EC_OPTION_MASK`).
- **`cd_values[1]`**: Pixels per block. Must be even and is typically
  `8`, `16`, or `32`.

**BZIP2 (`H5Z_FILTER_BZIP2`)**

- **Elements (`cd_nelmts`)**: 1
- **`cd_values[0]`**: Block size / Compression Level from `1` (fastest)
  to `9` (best). Default is `9`.

**LZF (`H5Z_FILTER_LZF`)**

- **Elements (`cd_nelmts`)**: 0 (No configuration parameters required).

**Blosc (`H5Z_FILTER_BLOSC`) & Blosc2 (`H5Z_FILTER_BLOSC2`)**

- **Elements (`cd_nelmts`)**: 7
- **`cd_values[0-3]`**: Reserved (Pass `0`).
- **`cd_values[4]`**: Compression level (`0` to `9`).
- **`cd_values[5]`**: Shuffle mode (`0` = no shuffle, `1` = byte
  shuffle, `2` = bit shuffle).
- **`cd_values[6]`**: Compressor ID (`0`=blosclz, `1`=lz4, `2`=lz4hc,
  `3`=snappy, `4`=zlib, `5`=zstd, `6`=zfp (Blosc2 only), `11`=ndlz
  (Blosc2 only)).

**Snappy (`H5Z_FILTER_SNAPPY`)**

- **Elements (`cd_nelmts`)**: 0 (No configuration parameters required).

**LZ4 (`H5Z_FILTER_LZ4`)**

- **Elements (`cd_nelmts`)**: 2
- **`cd_values[0]`**: Reserved/padding (Pass `0`).
- **`cd_values[1]`**: Compression level. Passing `0` uses standard fast
  LZ4. Passing a value `>0` (e.g., `9`) enables LZ4-HC (High
  Compression).

**Bitshuffle (`H5Z_FILTER_BSHUF`)**

- **Elements (`cd_nelmts`)**: 2
- **`cd_values[0]`**: Block size. Pass `0` to let the library choose the
  optimal default (1024).
- **`cd_values[1]`**: Compression algorithm. Pass `0` for raw
  bitshuffling (no compression) or `2` to apply LZ4 compression after
  shuffling.

**ZFP (`H5Z_FILTER_ZFP`)**

- **Restriction:** **Numeric arrays only.** Does not support strings,
  compound data types, or 1D arrays of bytes.
- **Elements (`cd_nelmts`)**: 6.
- **`cd_values[0]`**: The compression mode. `1`=Rate, `2`=Precision,
  `3`=Accuracy, `4`=Expert, `5`=Reversible (Lossless).
- **`cd_values[1-5]`**: Mode-specific parameters. For instance, in
  Precision mode (`cd_values[0]=2`), `cd_values[2]` specifies the bits
  of precision to keep, padding the rest with zeros (e.g.,
  `{2, 0, 16, 0, 0, 0}`).

**Zstandard / Zstd (`H5Z_FILTER_ZSTD`)**

- **Elements (`cd_nelmts`)**: 1
- **`cd_values[0]`**: Compression level, generally from `1` to `22`. A
  good default is `3`.

## Dynamically Loading Additional External Filters

While `hdf5lib` bundles an extensive suite of filters, it cannot include
every possible algorithm. For instance, high-performance filters written
in C++ like **SZ3** or **VBZ** are not bundled in order to maintain the
package’s strict C-only, zero-dependency compilation footprint.

However, HDF5 supports dynamically loading these algorithms at runtime
through its plugin mechanism. These plugins are shared libraries (`.so`
on Linux, `.dylib` on macOS, `.dll` on Windows) that the HDF5 library
can load when it encounters a compressed dataset.

`hdf5lib` is compiled with the necessary flags to enable this dynamic
loading feature.

### How It Works for the User

1.  **Install the Filter:** The user must obtain and install the desired
    filter plugin (like the SZ3 or VBZ plugin). These can often be
    compiled from source or are distributed by other scientific software
    packages (e.g., `h5py` for Python).
2.  **Set the Plugin Path:** The user must tell the HDF5 library where
    to find the installed plugins by setting the `HDF5_PLUGIN_PATH`
    environment variable.

Once the environment is configured, any R package that links to
`hdf5lib` (such as `h5lite` or your own package) will be able to read
and write datasets using that external filter without any changes to its
own C/C++ code.

For example, if a user has the SZ3 filter plugin installed in
`/opt/hdf5/plugins`, they can enable it in R like this:

``` r
# Tell HDF5 where to find filter plugins
Sys.setenv(HDF5_PLUGIN_PATH = "/opt/hdf5/plugins/")

# Now, any function that uses hdf5lib can read an SZ3-compressed file.
# For example, with 'h5lite' which links against hdf5lib:
# data <- h5lite::h5_read("my_sz3_file.h5", "my_dataset")
```

This powerful design means that as a package developer linking to
`hdf5lib`, you get native GZIP/SZIP and the bundled plugins for free,
while your package automatically retains the ability to work with any
external compression filter your users have installed.
