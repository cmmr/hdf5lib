# Getting Started

## Introduction

This article is for R package developers who want to use the HDF5 C
library in their own package. `hdf5lib` makes this easy by providing a
reliable, self-contained HDF5 build that you can link to without
requiring your users to install any system dependencies.

We will walk through the four main steps to link your package to
`hdf5lib` and then demonstrate a complete example using `Rcpp`.

## The 4 Steps to Link Your Package

To use `hdf5lib` in your package, you need to:

1.  Declare the dependency in your `DESCRIPTION` file.
2.  Tell the compiler where to find the `hdf5lib` headers and libraries
    in a `src/Makevars` file.
3.  Manage the global filter state to initialize compression plugins.
4.  Include the HDF5 headers in your C/C++ code.

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
PKG_CPPFLAGS = `$(R_HOME)/bin/Rscript -e "cat(hdf5lib::c_flags(api = 2.0))"`

# Get linker flags (e.g., -L/path/to/libs -lhdf5z)
PKG_LIBS     = `$(R_HOME)/bin/Rscript -e "cat(hdf5lib::ld_flags(api = 2.0))"`
```

### Step 3: Manage Global Filter State (Important)

To utilize the bundled compression plugins (LZ4, Zstd, Blosc, etc.),
they must be registered with the HDF5 library. Registering them changes
process-wide state (HDF5’s filter table and Blosc2’s codec registry), so
you should **never** register filters on a per-I/O basis. Tearing them
down with `hdf5lib_destroy_all_filters()` also frees the Blosc worker
threads and cached contexts that make multi-threaded Blosc fast, so
calling it after each operation would throw them away. Instead, call
them exactly once from your package’s DLL load/unload entry points.

**Register in `src/init.c`:**

``` c
#include <Rinternals.h>
#include <R_ext/Rdynload.h>
#include <hdf5lib.h>

static const R_CallMethodDef CallEntries[] = {
  // Add your package's own .Call entry points here
  {NULL, NULL, 0}
};

void R_init_myhdf5package(DllInfo *dll) {
  R_registerRoutines(dll, NULL, CallEntries, NULL, NULL);
  R_useDynamicSymbols(dll, FALSE);

  /* Register the compression plugins once, at load time */
  hdf5lib_register_all_filters();
}

void R_unload_myhdf5package(DllInfo *dll) {
  /* Free Blosc's worker threads and cached contexts, and unregister the filters */
  hdf5lib_destroy_all_filters();
}
```

`R_init_<pkgname>()` runs the moment your shared object is loaded, so
the filters are in place before any of your code can run.
`R_unload_<pkgname>()` only runs if the DLL is actually unloaded, which
R does not do by default – add this to `R/zzz.R` if you want the
teardown to happen:

``` r

.onUnload <- function(libpath) {
    library.dynam.unload("myhdf5package", libpath)
}
```

### Step 4: Include HDF5 Headers

You can now include the HDF5 headers directly in your C or C++ source
files located in the `src/` directory.

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

### C++ Source Code (`src/hdf5_helpers.cpp`)

``` cpp
#include <Rcpp.h>
#include <hdf5.h>
#include <string>
#include <vector>

//' Get the version of the linked HDF5 library
//'
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

1.  Run `Rcpp::compileAttributes()` to generate the Rcpp export files.
2.  Build and install your package.

Now, you can use your new function from R:

``` r

library(myhdf5package)
get_hdf5_version()
#> "2.2.0"
```
