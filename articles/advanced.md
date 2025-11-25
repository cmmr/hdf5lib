# Parallelism and Direct C-level Access

## Introduction

This vignette covers advanced and specialized uses of `hdf5lib`. The
topics include:

1.  **Parallel Programming with `RcppParallel`**: How to safely use HDF5
    from multiple threads.
2.  **Using `cpp11`**: An alternative to `Rcpp` for C++ integration.
3.  **Direct Dynamic Loading**: How to compile and call a C function
    that uses HDF5 directly from an R script, without creating a
    package. This is useful for quick tests or for users who need to
    access a specific HDF5 function not available in other packages.

## 1. Parallelism with `RcppParallel`

A key feature of `hdf5lib` is that it is compiled with HDF5’s
thread-safety mode enabled. This prevents data corruption when multiple
threads make calls to the HDF5 library simultaneously.

#### Important Limitations of Thread-Safety

- **Only Low-Level APIs are Thread-Safe:** The HDF5 library’s
  thread-safety guarantee **only applies to the low-level (LL)
  functions** (e.g., `H5F…`, `H5D…`, `H5P…`). The High-Level (HL) APIs,
  such as `H5LT`, `H5IM`, and `H5TB`, are **not** thread-safe and must
  not be called from multiple threads.
- **Threads vs. Processes:** This feature provides safety for multiple
  *threads* within a single R process. It does **not** protect against
  multiple independent *processes* writing to the same HDF5 file at the
  same time. To manage multi-process access, you must use an external
  mechanism like file locking (e.g., via the R package `flock`).

Here is an example of a function that uses `RcppParallel` to write to
different datasets within the same HDF5 file in parallel.

#### `DESCRIPTION` and `Makevars`

First, add `RcppParallel` to the `LinkingTo` field in your `DESCRIPTION`
file. Your `src/Makevars` file remains the same as in the “Getting
Started” vignette.

``` yaml
LinkingTo: hdf5lib, Rcpp, RcppParallel
```

#### C++ Code with `RcppParallel`

The following C++ code defines a “worker” struct that writes a simple
dataset using the thread-safe low-level API. `RcppParallel::parallelFor`
executes this worker on multiple threads, with each thread writing to a
different dataset.

``` cpp
#include <Rcpp.h>
#include <RcppParallel.h>
#include <hdf5.h>

struct H5Writer : public RcppParallel::Worker {
    const std::string filename;

    H5Writer(const std::string filename) : filename(filename) {}

    void operator()(std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; i++) {
            // Each thread works on a different dataset
            std::string dset_name = "thread_" + std::to_string(i);
            int data = static_cast<int>(i);
            hsize_t dims = {1};

            // HDF5's thread-safety guarantees these LL calls are safe
            hid_t file_id = H5Fopen(filename.c_str(), H5F_ACC_RDWR, H5P_DEFAULT);
            if (file_id < 0) continue;

            hid_t space_id = H5Screate_simple(1, dims, NULL);
            hid_t dset_id = H5Dcreate2(file_id, dset_name.c_str(), H5T_NATIVE_INT,
                                     space_id, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);

            if (dset_id >= 0) {
                H5Dwrite(dset_id, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, &data);
                H5Dclose(dset_id);
            }

            H5Sclose(space_id);
            H5Fclose(file_id);
        }
    }
};

//' Write to an HDF5 file in parallel
//' @param filename Path to the HDF5 file.
//' @param n_threads Number of threads to use.
//' @export
// [[Rcpp::export]]
void parallel_write(std::string filename, int n_threads) {
    // Create the file first
    hid_t file_id = H5Fcreate(filename.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
    if (file_id < 0) Rcpp::stop("Failed to create file.");
    H5Fclose(file_id);

    H5Writer writer(filename);
    RcppParallel::parallelFor(0, n_threads, writer);
}
```

Because `hdf5lib` enables thread-safety, the internal mutexes within the
HDF5 library will serialize the low-level API calls, preventing a race
condition.

## 2. Using `cpp11`

`cpp11` is a modern alternative to `Rcpp`. The setup is very similar.

1.  **`DESCRIPTION`**: Add `cpp11` to `LinkingTo`.
2.  **`src/Makevars`**: The file remains identical.
3.  **C++ Code**: Write your code using `cpp11` headers and conventions.

Here is the version-checking function from the “Getting Started” guide,
rewritten for `cpp11`.

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

After running `cpp11::cpp_register()`, you can build the package and
call the function from R.

## 3. Direct Dynamic Loading (No Package Needed)

Sometimes you may want to run a small piece of C code that uses HDF5
without the overhead of creating a full R package. You can do this by
compiling the C code into a shared library (`.so` or `.dll`) on the fly
and loading it directly into your R session.

This is exactly how `hdf5lib`’s own internal tests work.

### Step 1: Write the C Source File

Save your C code to a file, for example, `get_version.c`. This function
must be compatible with R’s
[`.Call()`](https://rdrr.io/r/base/CallExternal.html) interface, meaning
it must take `SEXP` arguments and return a `SEXP`.

``` c
// In file: get_version.c
#include <R.h>
#include <Rinternals.h>
#include <hdf5.h>
#include <stdio.h> // for snprintf

SEXP get_version_c() {
    unsigned maj, min, rel;
    char version_str;

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
[`system()`](https://rdrr.io/r/base/system.html) call, using the helper
functions from `hdf5lib`.

``` r
# Ensure hdf5lib is installed
if (!require("hdf5lib")) install.packages("hdf5lib")

c_file <- "get_version.c"
so_file <- sub("\\\\.c$", .Platform$dynlib.ext, c_file)

# Set environment variables for the compiler
Sys.setenv(
  PKG_CPPFLAGS = hdf5lib::c_flags(),
  PKG_LIBS = hdf5lib::ld_flags()
)

# Construct and run the compilation command
R_EXE <- file.path(R.home("bin"), "R")
compile_cmd <- sprintf('%s CMD SHLIB %s', shQuote(R_EXE), shQuote(c_file))

cat("Compiling with command:\n", compile_cmd, "\n")
system(compile_cmd)

# Clean up environment variables
Sys.unsetenv(c("PKG_CPPFLAGS", "PKG_LIBS"))

# Load the shared library and call the C function
dyn.load(so_file)
version <- .Call("get_version_c")

print(version)

# Unload the library
dyn.unload(so_file)
```

This powerful technique gives you direct access to the full HDF5 C API
from a simple R script, which is ideal for one-off tasks, debugging, or
accessing niche functions not exposed by other R packages.
