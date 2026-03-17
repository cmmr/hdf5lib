# Comparison

The R ecosystem has two main packages that provide the HDF5 C library to
other packages: `hdf5lib` (this package) and `Rhdf5lib` from
Bioconductor. While both serve a similar purpose, they are built with
different design philosophies and technical trade-offs. This article
compares them to help you decide which is the right choice for your
project.

## At a Glance

The table below summarizes the key differences between the two packages.

| Feature                 | `hdf5lib` (CRAN)                                                                        | `Rhdf5lib` (Bioconductor)                |
|-------------------------|-----------------------------------------------------------------------------------------|------------------------------------------|
| **Installation**        | Zero-configuration                                                                      | Configurable at install-time             |
| **HDF5 Version**        | Bundled & predictable (v2.1.0)                                                          | Variable (older bundle or system)        |
| **Compression Plugins** | **Bundled (Blosc2, Zstd, LZ4, etc.)**                                                   | Requires external/system libraries       |
| **Build Type**          | Static library only                                                                     | Static or shared library                 |
| **Thread-Safety**       | **Enabled by default**                                                                  | Not supported                            |
| **API Version Control** | Simple `api` flag in [`c_flags()`](https://cmmr.github.io/hdf5lib/reference/c_flags.md) | Requires manual `-D` flags               |
| **Ecosystem**           | General purpose                                                                         | Primarily for the Bioconductor ecosystem |

## Key Differences Explained

### 1. Installation and Configuration

- **`hdf5lib`** is designed for simplicity. Installation via
  `install.packages("hdf5lib")` is a “zero-config” process. It always
  builds its bundled HDF5 source code with a standard, modern feature
  set (including thread-safety, high-level APIs, and bundled compression
  filters). This ensures a consistent and reliable build for downstream
  packages with no effort from the user.

- **`Rhdf5lib`** is designed for flexibility. It provides several
  `configure` arguments that allow users to customize the HDF5 build.
  For example, users can choose to link against a system-installed HDF5
  library or disable features like the high-level APIs. While powerful,
  this flexibility means that a downstream package developer cannot be
  certain which HDF5 features will be available on a user’s machine.

### 2. HDF5 Version

- **`hdf5lib`** bundles a modern version of HDF5 (v2.1.0). This gives
  developers immediate access to the latest features, bug fixes, and
  performance improvements, such as native complex number support and
  better UTF-8 handling on Windows.

- **`Rhdf5lib`** typically bundles an older, long-term support version
  of HDF5 that is standard across the Bioconductor ecosystem (v1.12.2 as
  of Bioconductor 3.19). This prioritizes stability within that
  ecosystem over access to the newest features.

### 3. Compression Filters

- **`hdf5lib`** bundles a wide array of state-of-the-art compression
  plugins directly into the package (Blosc2, Zstd, LZ4, Snappy, ZFP,
  etc.), making them universally available to any package linking to it
  without requiring system-level dependencies.

- **`Rhdf5lib`** provides standard native compression (gzip/szip), but
  leveraging modern codecs typically requires the user to install them
  externally on their system.

### 4. Thread-Safety

- **`hdf5lib`** builds HDF5 with thread-safety enabled by default. This
  is a critical feature for modern R packages that use parallelism
  (e.g., via `RcppParallel`), as it prevents data corruption when
  multiple threads access an HDF5 file. For more details, see the
  article on [Parallel
  Programming](https://cmmr.github.io/hdf5lib/articles/parallelism.md).

- **`Rhdf5lib`** does not support building with thread-safety enabled.

### 5. API Version Control

- **`hdf5lib`** provides a simple `api` argument in its
  [`c_flags()`](https://cmmr.github.io/hdf5lib/reference/c_flags.md) and
  [`ld_flags()`](https://cmmr.github.io/hdf5lib/reference/ld_flags.md)
  helper functions. This makes it trivial for a developer to lock their
  package to a specific HDF5 API version (e.g., `api = 1.14`), ensuring
  that future updates to `hdf5lib` will not break their package. This
  feature is explained in detail in the [API
  Versioning](https://cmmr.github.io/hdf5lib/articles/api-versioning.md)
  article.

- **`Rhdf5lib`** does not provide a dedicated mechanism for this. A
  developer would need to manually add the appropriate `-D` flags to
  their `Makevars`, and the underlying library may not contain the
  symbols for all API versions.

### 6. Versioning and Predictability

- **`hdf5lib`** offers a predictable dependency. The version of the
  `hdf5lib` package itself tells you the version of the underlying HDF5
  C library. For example, `hdf5lib` version `2.1.0.x` bundles HDF5
  version `2.1.0`. This allows a downstream package developer to specify
  a minimum `hdf5lib` version in their `DESCRIPTION` file (e.g.,
  `hdf5lib (>= 2.1.0)`) and be certain that they are working with at
  least HDF5 v2.1.0 and that key features like thread-safety are
  enabled.

- **`Rhdf5lib`**’s package version is not correlated with the version of
  the HDF5 C library that is ultimately used. Because `Rhdf5lib` will
  preferentially link against an HDF5 library already installed on the
  user’s system, or use configuration arguments passed during
  installation, a developer has no guarantee about what HDF5 version is
  present or what features it was built with. This can lead to
  portability issues where a package works for one user but fails for
  another who has a different HDF5 configuration.

## Conclusion: Which Should You Use?

Choose **`hdf5lib`** if:

- You want a simple, reliable dependency that “just works” for you and
  your users.
- You need access to modern, state-of-the-art compression plugins
  out-of-the-box.
- You plan to use multithreading (e.g., with `RcppParallel`).
- You want to lock your package to a specific API version for long-term
  stability.

Choose **`Rhdf5lib`** if:

- Your package is part of the Bioconductor ecosystem and you need to
  maintain strict compatibility with it.
- You need to link against a specific, older configuration of HDF5 that
  is not met by `hdf5lib`’s default build.
