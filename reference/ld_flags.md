# Get C/C++ Linker Flags for hdf5lib

Provides the required linker flags to link against the static HDF5
library (\`libhdf5z.a\`) bundled with the \`hdf5lib\` package.

This function is intended to be called from a \`Makevars\` file by other
R packages that link to \`hdf5lib\`. It returns the \`-L\` path to the
library directory and the \`-l\` flags for \`libhdf5\` and its system
dependencies (like \`pthread\` and \`dl\`).

## Usage

``` r
ld_flags()
```

## Value

A scalar character vector containing the linker flags.

## See also

\[c_flags()\]

## Examples

``` r
ld_flags()
#> [1] "-L/home/runner/work/_temp/Library/hdf5lib/lib -lhdf5z -lpthread -ldl"
```
