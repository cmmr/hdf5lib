# hdf5lib: A Standalone HDF5 C Library for R


`hdf5lib` is an R package that provides a self-contained, static build of the HDF5 C library. Its **sole purpose** is to allow other R packages to easily link against HDF5 without requiring users to install any system-level dependencies.

This package provides **no R functions** and is intended for R package developers to use in the `LinkingTo` field of their `DESCRIPTION` file.

It solves the common and frustrating problem of installation failures on Windows, macOS, and Linux by bundling the required C libraries (HDF5 and zlib) directly into the package.

## Features

* **Self-contained:** Builds the HDF5 library from source, requiring only R and a standard C compiler (like Rtools on Windows).
* **No System Dependencies:** Users can install your package without first needing to `apt-get install` or `brew install` HDF5 or zlib.
* **Zlib Support:** Includes `zlib` support, enabling your package to read and write the most common type of compressed HDF5 files.

### Bundled Libraries

* **HDF5** version 1.14.6
* **zlib** version 1.3.1

---

## Installation

This package is not yet on CRAN. You can install the development version from GitHub:

```r
# install.packages("devtools")
devtools::install_github("cmmr/hdf5lib")
```

**Note:** As this package builds both HDF5 and zlib from source, the one-time installation may take a few minutes.

---

## Usage (For Developers)

To use this library in your own R package, you simply need to link to it.

### 1. Update your `DESCRIPTION` file

Add `hdf5lib` to the `LinkingTo` field.

```
Package: myrpackage
Version: 0.1.0
...
LinkingTo: hdf5lib
```

### 2. Include the Header in Your C/C++ Code

You can now include the HDF5 headers directly in your package's `src` files. The R build system will automatically find them.

For example, in `src/my_c_code.c`:

```c
#include <R.h>
#include <Rinternals.h>

// This is all you need!
#include <hdf5.h>

SEXP read_my_hdf5_data(SEXP filename) {
    hid_t file_id;
    const char *fname = CHAR(STRING_ELT(filename, 0));

    // Call HDF5 functions directly
    file_id = H5Fopen(fname, H5F_ACC_RDONLY, H5P_DEFAULT);

    // ... your code ...

    H5Fclose(file_id);
    return R_NilValue;
}
```

---

## License

The `hdf5lib` package itself is available under the MIT license.

The bundled libraries are available under their own permissive licenses:
* **HDF5:** [HDF5 License](https://github.com/HDFGroup/hdf5/blob/develop/COPYING)
* **zlib:** [zlib License](https://github.com/madler/zlib/blob/master/LICENSE)