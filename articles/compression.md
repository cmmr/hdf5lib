# Compression & Filters

One of the most powerful features of HDF5 is its ability to compress
data transparently. When you read a compressed dataset, the HDF5 library
automatically decompresses it for you. This can lead to significant
savings in storage space and faster I/O performance by reducing the
amount of data that needs to be transferred from disk.

`hdf5lib` provides built-in support for `gzip` compression and enables
the HDF5 library’s dynamic filter mechanism, allowing users to leverage
a wide range of other compression algorithms.

## Built-in Compression (gzip/deflate)

`hdf5lib` bundles the `zlib` library, which provides the `deflate`
(commonly known as `gzip`) compression algorithm. This means that any
package linking to `hdf5lib` can create and read `gzip`-compressed
datasets out-of-the-box, with no extra configuration.

To create a compressed dataset, you must do two things:

1.  **Enable Chunking:** Compression in HDF5 requires the data to be
    stored in “chunks.” You must define a chunk size for your dataset.
2.  **Set the Filter:** You must add the `deflate` filter to the dataset
    creation property list.

### C++ Example

The following `Rcpp` example demonstrates how to create a chunked and
compressed dataset.

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

## External Filters (Blosc, LZ4, etc.)

HDF5 supports a much wider range of compression algorithms (e.g., Blosc,
LZ4, Bzip2) through a dynamic plugin mechanism. These plugins are shared
libraries (`.so` on Linux, `.dylib` on macOS, `.dll` on Windows) that
the HDF5 library can load at runtime to handle new compression formats.

`hdf5lib` is compiled with the necessary flags to enable this dynamic
loading feature. However, **`hdf5lib` does not bundle any external
filters.** It is the end-user’s responsibility to install them.

### How It Works for the User

1.  **Install the Filter:** The user must obtain and install the desired
    filter plugin. These are often distributed by other scientific
    software packages (e.g., `h5py` for Python) or can be compiled from
    source.

2.  **Set the Plugin Path:** The user must tell the HDF5 library where
    to find the installed plugins by setting the `HDF5_PLUGIN_PATH`
    environment variable.

Once the environment is configured, any R package that links to
`hdf5lib` (such as `h5lite` or your own package) will be able to read
and write datasets using that filter without any changes to its own
code.

For example, if a user has the Blosc filter plugin installed in
`/opt/hdf5/plugins`, they can enable it in R like this:

``` r
# Tell HDF5 where to find filter plugins
Sys.setenv(HDF5_PLUGIN_PATH = "/opt/hdf5/plugins/")

# Now, any function that uses hdf5lib can read a Blosc-compressed file.
# For example, with 'h5lite' which links against hdf5lib:
# data <- h5lite::h5_read("my_blosc_file.h5", "my_dataset")
```

This powerful design means that as a package developer linking to
`hdf5lib`, you get `gzip` support for free, and your package
automatically gains the ability to work with any compression filter your
users have installed.
