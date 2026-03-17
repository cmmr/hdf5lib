# API Versioning

The HDF5 library maintains excellent backward compatibility. When
function signatures or data structures change, the old versions are
often kept available. A key feature of `hdf5lib` is that it exposes an
easy way to compile your C/C++ code against a specific, older version of
the HDF5 API.

## Why Target a Specific API?

The primary reason is **stability**. By locking your package to a
specific HDF5 API version, you prevent your package from breaking
unexpectedly when `hdf5lib` is updated with a new version of HDF5. This
gives you, the package developer, control over the upgrade cycle. You
can choose to increase the API version when you are ready to test your
code against newer HDF5 features and behaviors.

## How to Set the API Version

You can control the API version by passing the `api` argument to
[`hdf5lib::c_flags()`](https://cmmr.github.io/hdf5lib/reference/c_flags.md)
and
[`hdf5lib::ld_flags()`](https://cmmr.github.io/hdf5lib/reference/ld_flags.md)
in your package’s `src/Makevars` file. This automatically adds the
correct preprocessor directive (e.g., `-DH5_USE_114_API_DEFAULT`) during
compilation.

For example, to ensure your code uses the HDF5 1.14 API, you would
modify `src/Makevars` as follows:

``` makefile
# In your package's src/Makevars
PKG_CPPFLAGS = `$(R_HOME)/bin/Rscript -e "cat(hdf5lib::c_flags(api = 1.14))"`
PKG_LIBS     = `$(R_HOME)/bin/Rscript -e "cat(hdf5lib::ld_flags(api = 1.14))"`
```

The HDF5 headers included via `hdf5lib` will then expose the
1.14-compatible function signatures. Because the static library bundled
by `hdf5lib` contains the implementations for multiple API versions,
your code will compile and link correctly.

If you want to use the newest available API, you can simply omit the
`api` argument.

### Supported Versions

The `api` argument accepts the following values:

- `2.0` (HDF5 2.1.x)
- `1.14` (HDF5 1.14.x)
- `1.12` (HDF5 1.12.x)
- `1.10` (HDF5 1.10.x)
- `1.8` (HDF5 1.8.x)
- `1.6` (HDF5 1.6.x)

> **Note:** You must choose from the explicit list above. There is no
> `api = 2.1` option, as the HDF5 library does not utilize an
> `H5_USE_210_API_DEFAULT` macro internally. To build against the 2.1.x
> series, use `api = 2.0` or simply omit the argument.
