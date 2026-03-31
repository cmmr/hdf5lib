# Get C/C++ Linker Flags for hdf5lib

Provides the required linker flags to link against the static HDF5
library (`libhdf5z.a`) bundled with the `hdf5lib` package.

## Usage

``` r
ld_flags(api = "latest")
```

## Arguments

- api:

  A numeric value or the string `"latest"`. This parameter is included
  for consistency with
  [`c_flags()`](https://cmmr.github.io/hdf5lib/reference/c_flags.md) and
  is reserved for future use; it currently has no effect on the linker
  flags. Defaults to `"latest"`.

## Value

A scalar character vector containing the linker flags.

## See also

[`c_flags()`](https://cmmr.github.io/hdf5lib/reference/c_flags.md)

## Examples

``` r
ld_flags()
#> [1] "-L/home/runner/work/_temp/Library/hdf5lib/lib -lhdf5z -lpthread -ldl"
```
