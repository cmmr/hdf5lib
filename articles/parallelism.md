# Parallel Programming

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
Started” article.

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
