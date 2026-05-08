# Integration Guide

This article contains a collection of practical examples for integrating
`hdf5lib` into your R package using R’s native C API, `Rcpp`, or
`cpp11`. It also includes a guide for direct dynamic loading if you want
to use HDF5 in a standalone R script.

> **Note:** Whenever you use the bundled advanced compression filters
> (anything outside of GZIP or SZIP), ensure you have registered them at
> the package level using `.onLoad` and `.onUnload` hooks as detailed in
> the Getting Started guide.

## 1. Using R’s Native C API

For many R package developers, writing native C code using R’s `.Call`
interface is the preferred way to interact with C libraries, as it
avoids the compilation overhead and abstraction layers of C++.

To use `hdf5lib` natively, ensure `LinkingTo: hdf5lib` is in your
`DESCRIPTION` file, and your `src/Makevars` is set up with the compiler
and linker flags.

The following example demonstrates a native C function designed to be
called from R. It creates an HDF5 file, sets up a dataset with the
Zstandard (Zstd) compression filter, and writes data to it.

``` c
#include <R.h>
#include <Rinternals.h>
#include <hdf5.h>

// Remember to call hdf5lib_register_all_filters() in your R package's .onLoad!

SEXP write_zstd_native(SEXP r_filename, SEXP r_dsetname) {
    const char *filename = CHAR(STRING_ELT(r_filename, 0));
    const char *dsetname = CHAR(STRING_ELT(r_dsetname, 0));
    
    int data[1000];
    for(int i = 0; i < 1000; i++) data[i] = i;
    hsize_t dims[1] = { 1000 };

    hid_t file_id = H5Fcreate(filename, H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
    hid_t space_id = H5Screate_simple(1, dims, NULL);
    hid_t dcpl_id = H5Pcreate(H5P_DATASET_CREATE);

    // 1. Enable chunking (Required for all filters)
    hsize_t chunk_dims[1] = { 100 };
    H5Pset_chunk(dcpl_id, 1, chunk_dims);

    // 2. Apply Zstandard Filter (Filter ID: 32015)
    // cd_values[0] = Compression Level (e.g., 5)
    unsigned int cd_values[1] = { 5 }; 
    H5Pset_filter(dcpl_id, 32015, H5Z_FLAG_MANDATORY, 1, cd_values);

    hid_t dset_id = H5Dcreate2(file_id, dsetname, H5T_NATIVE_INT,
                                space_id, H5P_DEFAULT, dcpl_id, H5P_DEFAULT);

    H5Dwrite(dset_id, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, data);

    H5Pclose(dcpl_id);
    H5Dclose(dset_id);
    H5Sclose(space_id);
    H5Fclose(file_id);
    
    return R_NilValue;
}
```

## 2. Using `Rcpp`

If you prefer working with C++, integrating `hdf5lib` is equally
straightforward. Make sure `LinkingTo: hdf5lib, Rcpp` is in your
`DESCRIPTION` file.

This example accomplishes the same Zstandard compression task as above,
but uses `Rcpp` features and standard C++ vectors.

``` cpp
#include <Rcpp.h>
#include <hdf5.h>
#include <vector>

// [[Rcpp::export]]
void write_zstd_rcpp(std::string filename, std::string dsetname) {
    std::vector<int> data(1000, 42);
    hsize_t dims[1] = { data.size() };

    hid_t file_id = H5Fcreate(filename.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
    hid_t space_id = H5Screate_simple(1, dims, NULL);
    hid_t dcpl_id = H5Pcreate(H5P_DATASET_CREATE);

    hsize_t chunk_dims[1] = { 100 };
    H5Pset_chunk(dcpl_id, 1, chunk_dims);

    unsigned int cd_values[1] = { 3 }; // Zstd Level 3
    H5Pset_filter(dcpl_id, 32015, H5Z_FLAG_MANDATORY, 1, cd_values);

    hid_t dset_id = H5Dcreate2(file_id, dsetname.c_str(), H5T_NATIVE_INT,
                                space_id, H5P_DEFAULT, dcpl_id, H5P_DEFAULT);

    H5Dwrite(dset_id, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, data.data());

    H5Pclose(dcpl_id);
    H5Dclose(dset_id);
    H5Sclose(space_id);
    H5Fclose(file_id);
}
```

## 3. Using `cpp11`

`cpp11` is a modern alternative to `Rcpp` for creating C++ bindings for
R. The setup is identical: add `cpp11` to the `LinkingTo` field and
retain the standard `src/Makevars`.

Here is a version-checking function written for `cpp11`.

``` cpp
#include "cpp11/strings.hpp"
#include "cpp11/function.hpp"
#include <hdf5.h>
#include <vector>

[[cpp11::register]]
cpp11::strings get_hdf5_version_cpp11() {
    unsigned int majnum, minnum, relnum;
    H5get_libversion(&majnum, &minnum, &relnum);

    std::vector<char> version_str(20);
    snprintf(version_str.data(), version_str.size(), "%u.%u.%u", majnum, minnum, relnum);

    return { std::string(version_str.data()) };
}
```

## 4. Direct Dynamic Loading (Scripting)

Sometimes you may want to run a small piece of C code that uses HDF5
without the overhead of creating an R package. You can compile C code
into a shared library on the fly and load it directly into your R
session.

### Step 1: Write the C Source File

``` c
// In file: get_version.c
#include <R.h>
#include <Rinternals.h>
#include <hdf5.h>
#include <stdio.h> 

SEXP get_version_c() {
    unsigned maj, min, rel;
    char version_str[20];

    H5get_libversion(&maj, &min, &rel);
    snprintf(version_str, sizeof(version_str), "%u.%u.%u", maj, min, rel);

    SEXP result = PROTECT(Rf_allocVector(STRSXP, 1));
    SET_STRING_ELT(result, 0, Rf_mkChar(version_str));
    UNPROTECT(1);

    return result;
}
```

### Step 2: Compile and Load in R

In your R script, use [`system()`](https://rdrr.io/r/base/system.html)
to call `R CMD SHLIB`. The key is to set the `PKG_CPPFLAGS` and
`PKG_LIBS` environment variables for the
[`system()`](https://rdrr.io/r/base/system.html) call using `hdf5lib`’s
helper functions.

``` r

if (!require("hdf5lib")) install.packages("hdf5lib")

c_file <- "get_version.c"
so_file <- paste0("get_version", .Platform$dynlib.ext)

# Set environment variables for the compiler
Sys.setenv(
    PKG_CPPFLAGS = hdf5lib::c_flags(),
    PKG_LIBS = hdf5lib::ld_flags()
)

# Construct and run the compilation command
R_EXE <- file.path(R.home("bin"), "R")
compile_cmd <- sprintf('%s CMD SHLIB %s', shQuote(R_EXE), shQuote(c_file))

system(compile_cmd)

# Clean up environment variables
Sys.unsetenv(c("PKG_CPPFLAGS", "PKG_LIBS"))

# Load the shared library and call the C function
dyn.load(so_file)
version <- .Call("get_version_c")

print(version)
dyn.unload(so_file)
```
