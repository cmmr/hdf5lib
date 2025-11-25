# Get C/C++ Compiler Flags for hdf5lib

Provides the required C/C++ compiler flags to find the HDF5 header files
bundled with the \`hdf5lib\` package.

This function is intended to be called from a \`Makevars\` file by other
R packages that link to \`hdf5lib\`.

## Usage

``` r
c_flags()
```

## Value

A scalar character vector containing the compiler flags (e.g., the
\`-I\` path to the package's \`inst/include\` directory).

## See also

\[ld_flags()\]

## Examples

``` r
c_flags()
#> [1] "-I/home/runner/work/_temp/Library/hdf5lib/include -DH5_BUILT_AS_STATIC_LIB"
```
