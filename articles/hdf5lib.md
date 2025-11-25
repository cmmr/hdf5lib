# Using hdf5lib in Your R Package

## Introduction

This vignette is for R package developers who want to use the HDF5 C
library in their own package. `hdf5lib` makes this easy by providing a
reliable, self-contained HDF5 build that you can link to without
requiring your users to install any system dependencies.

We will walk through the three main steps to link your package to
`hdf5lib` and then demonstrate a complete example using `Rcpp`.

## The 3 Steps to Link Your Package

To use `hdf5lib` in your package, you need to:

1.  Declare the dependency in your `DESCRIPTION` file.
2.  Tell the compiler where to find the `hdf5lib` headers and libraries
    in a `src/Makevars` file.
3.  Include the HDF5 headers in your C/C++ code.

### Step 1: Update `DESCRIPTION`

Add `hdf5lib` to the `LinkingTo` field in your `DESCRIPTION` file. This
tells R that your package needs to access header files from `hdf5lib`.

``` yaml
Package: myhdf5package
Version: 0.1.0
LinkingTo: hdf5lib, Rcpp
```

*(We’ve also added `Rcpp` because we’ll use it in our example below.)*

### Step 2: Create `src/Makevars`

Create a file named `Makevars` inside your package’s `src/` directory.
This file provides instructions to the compiler. `hdf5lib` provides two
helper functions,
[`c_flags()`](https://cmmr.github.io/hdf5lib/reference/c_flags.md) and
[`ld_flags()`](https://cmmr.github.io/hdf5lib/reference/ld_flags.md),
that output the exact strings needed.

Add the following lines to `src/Makevars`:

``` makefile
# Get compiler flags (e.g., -I/path/to/headers)
PKG_CPPFLAGS = `$(R_HOME)/bin/Rscript -e "cat(hdf5lib::c_flags())"`

# Get linker flags (e.g., -L/path/to/libs -lhdf5)
PKG_LIBS     = `$(R_HOME)/bin/Rscript -e "cat(hdf5lib::ld_flags())"`
```

These commands run a short R script during compilation to fetch the
correct flags. This approach is platform-independent and works on
Windows, macOS, and Linux without any changes.

### Step 3: Include HDF5 Headers

You can now include the HDF5 headers directly in your C or C++ source
files located in the `src/` directory.

The main header is `<hdf5.h>`. For the high-level APIs (which are highly
recommended), you should also include `<hdf5_hl.h>`.

``` cpp
#include <Rcpp.h>
#include <hdf5.h>
#include <hdf5_hl.h>

// Your code using HDF5 functions goes here...
```

## Example: Creating a Package with `Rcpp`

Let’s create a minimal R package that uses `Rcpp` to provide one
function: `get_hdf5_version()`, which calls the HDF5 C library and
returns its version string.

### Package Structure

First, create a new package. You can use
`Rcpp::Rcpp.package.skeleton("myhdf5package")` as a starting point. The
file structure will look like this:

    myhdf5package/
    ├── DESCRIPTION
    ├── NAMESPACE
    ├── R/
    │   └── RcppExports.R
    ├── src/
    │   ├── Makevars
    │   ├── RcppExports.cpp
    │   └── hdf5_helpers.cpp  // Our new C++ file
    └── man/

### `DESCRIPTION` and `Makevars`

Update `DESCRIPTION` and create `src/Makevars` as described in Steps 1
and 2.

### C++ Source Code (`src/hdf5_helpers.cpp`)

This file contains the C++ function that will be callable from R. It
uses `H5get_libversion` to query the library version and returns it as a
string.

``` cpp
#include <Rcpp.h>
#include <hdf5.h>
#include <string>
#include <vector>

//' Get the version of the linked HDF5 library
 //'
 //' This function calls the HDF5 C API to retrieve the library version
 //' and returns it as a character string (e.g., "2.0.0").
 //' @export
 // [[Rcpp::export]]
Rcpp::String get_hdf5_version() {
    unsigned int majnum, minnum, relnum;

    // Call the HDF5 C function
    herr_t status = H5get_libversion(&majnum, &minnum, &relnum);

    if (status < 0) {
        Rcpp::stop("Failed to get HDF5 library version.");
    }

    // Format the version string
    std::vector<char> version_str(20);
    snprintf(version_str.data(), version_str.size(), "%u.%u.%u", majnum, minnum, relnum);

    return Rcpp::String(version_str.data());
}
```

### Build and Run

1.  Run `Rcpp::compileAttributes()` to generate the Rcpp export files
    (`R/RcppExports.R` and `src/RcppExports.cpp`).
2.  Build and install your package using `devtools::install()` or by
    building a source tarball.

Now, you can use your new function from R:

``` r
library(myhdf5package)
get_hdf5_version()
#> "2.0.0"
```

This simple example demonstrates that your package is successfully
linking to `hdf5lib` and can call its functions. You can now build more
complex functionality by expanding on this foundation.
