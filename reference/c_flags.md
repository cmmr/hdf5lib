# Get C/C++ Compiler Flags for hdf5lib

Provides the required C/C++ compiler flags to find the HDF5 header files
bundled with the `hdf5lib` package.

## Usage

``` r
c_flags(api = "latest")
```

## Arguments

- api:

  A numeric value specifying the HDF5 API version to use (e.g., `114`
  for v1.14), or the string `"latest"`. This adds a preprocessor
  directive like `-DH5_USE_114_API_DEFAULT` to ensure that the compiled
  code uses symbols compatible with a specific version of the HDF5 API.
  This is useful for maintaining compatibility with older HDF5 versions.
  Supported values are `200`, `114`, `112`, `110`, `18`, and `16`.
  Defaults to `"latest"`, which corresponds to the newest supported API
  version.

## Value

A scalar character vector containing the compiler flags (e.g., the `-I`
path to the package's `inst/include` directory).

## See also

[`ld_flags()`](https://cmmr.github.io/hdf5lib/reference/ld_flags.md)

## Examples

``` r
c_flags()
#> [1] "-I/home/runner/work/_temp/Library/hdf5lib/include -DH5_BUILT_AS_STATIC_LIB -DH5_USE_200_API_DEFAULT"
c_flags(api = "114")
#> [1] "-I/home/runner/work/_temp/Library/hdf5lib/include -DH5_BUILT_AS_STATIC_LIB -DH5_USE_114_API_DEFAULT"
```
