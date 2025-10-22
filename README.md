# **hdf5lib: Standalone HDF5 C Library**

`hdf5lib` is an R package that provides a self-contained, static build of the [HDF5 C library](https://www.hdfgroup.org/solutions/hdf5/) ([release 1.14.6](https://github.com/HDFGroup/hdf5)). Its **sole purpose** is to allow other R packages to easily link against HDF5 without requiring users to install any system-level dependencies.

This package provides **no R functions** and is intended for R package developers to use in the `LinkingTo` field of their `DESCRIPTION` file.

It solves the common and frustrating problem of installation failures on Windows, macOS, and Linux by bundling the required HDF5 C library directly into the package.

## Features

* **Self-contained:** Builds the HDF5 library from source, requiring only R and a standard C compiler (like Rtools on Windows, Xcode Command Line Tools on macOS, or `build-essential` on Linux).

* **No System Dependencies:** Users can install your package without first needing to `apt-get install` or `brew install` HDF5.

* **Includes High-Level API:** Provides both the core HDF5 C API and the convenient High-Level (HL) APIs, including H5LT (Lite), H5IM (Image), and H5TB (Table).

* **Compression Support:** The bundled HDF5 library is built with zlib support enabled, allowing linked packages to read and write HDF5 files using standard `gzip/deflate` compression.

### Bundled Library

* **HDF5** [release 1.14.6](https://github.com/HDFGroup/hdf5) (including HL APIs)

## **Installation**

You can install the released version of `hdf5lib` from CRAN with:

```r
install.packages("hdf5lib")
```

Alternatively, you can install the development version from GitHub:

```r
# install.packages("devtools")  
devtools::install_github("cmmr/hdf5lib")
```

**Note:** As this package builds the HDF5 library from source, the one-time installation may take several minutes. ⏳

## **Usage (For Developers)**

To use this library in your own R package, you simply need to link to it.

### **1. Update your DESCRIPTION file**

Add `hdf5lib` to the `LinkingTo` field.  

```yaml
Package: myrpackage  
Version: 0.1.0  
...  
LinkingTo: hdf5lib
```

### **2. Include the Header in Your C/C++ Code**

You can now include the HDF5 headers directly in your package's `src` files. The R build system will automatically find them.  

```c
#include <R.h>  
#include <Rinternals.h>

// Include the main HDF5 header  
#include <hdf5.h>

// Optionally include the High-Level header for H5LT etc.  
#include <hdf5_hl.h>

SEXP read_my_hdf5_data(SEXP filename) {  
    hid_t file_id;  
    const char *fname = CHAR(STRING_ELT(filename, 0));

    // Call HDF5 functions directly  
    file_id = H5Fopen(fname, H5F_ACC_RDONLY, H5P_DEFAULT);

    // ... your code using HDF5 APIs ...

    H5Fclose(file_id);  
    return R_NilValue;  
}
```

## **Included HDF5 APIs**

This package provides access to the HDF5 C API, including:

### **High-Level (HL) APIs (Recommended for simplicity)**

* **H5LT (Lite):** Simplified functions for common dataset and attribute operations.  
  * `H5LTmake_dataset_int()`, `H5LTmake_dataset_double()`, etc.  
  * `H5LTread_dataset_int()`, `H5LTread_dataset_double()`, etc.  
  * `H5LTset_attribute_string()`, `H5LTget_attribute_int()`, etc.  
  * `H5LTget_dataset_info()`  
* **H5IM (Image):** Functions for working with image data.  
  * `H5IMmake_image_24bit()`, `H5IMread_image()`  
* **H5TB (Table):** Functions for working with table structures.  
  * `H5TBmake_table()`, `H5TBappend_records()`, `H5TBread_records()`

### **Low-Level APIs (Core functionality for fine-grained control)**

* **H5F (File):** `H5Fcreate()`, `H5Fopen()`, `H5Fclose()`  
* **H5G (Group):** `H5Gcreate2()`, `H5Gopen2()`, `H5Gclose()`  
* **H5D (Dataset):** `H5Dcreate2()`, `H5Dopen2()`, `H5Dread()`, `H5Dwrite()`, `H5Dclose()`  
* **H5S (Dataspace):** `H5Screate_simple()`, `H5Sselect_hyperslab()`, `H5Sclose()`  
* **H5T (Datatype):** `H5Tcopy()`, `H5Tset_size()`, `H5Tinsert()`, `H5Tclose()` (and predefined types like `H5T_NATIVE_INT`, `H5T_NATIVE_DOUBLE`)  
* **H5A (Attribute):** `H5Acreate2()`, `H5Aopen()`, `H5Aread()`, `H5Awrite()`, `H5Aclose()`  
* **H5P (Property List):** `H5Pcreate()`, `H5Pset_chunk()`, `H5Pset_deflate()`, `H5Pclose()`

For complete documentation, see the official HDF5 Reference Manual:  
<https://support.hdfgroup.org/documentation/hdf5/latest/_r_m.html>

## **License**

The `hdf5lib` package itself is available under the MIT license.

The bundled HDF5 and zlib libraries are available under their own permissive licenses:

* **HDF5:** [HDF5 License](https://github.com/cmmr/hdf5lib/blob/main/inst/licenses/hdf5-LICENSE.txt)
* **zlib:** [zlib License](https://github.com/cmmr/hdf5lib/blob/main/inst/licenses/zlib-LICENSE.txt)

*(Note: The zlib library is bundled internally but its headers are not exposed).*