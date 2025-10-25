#' Get C/C++ Compiler Flags for hdf5lib
#'
#' @description
#' Provides the required C/C++ compiler flags to find the HDF5 header
#' files bundled with the `hdf5lib` package.
#'
#' This function is intended to be called from a `Makevars` file by
#' other R packages that link to `hdf5lib`.
#'
#' @return A scalar character vector containing the compiler flags (e.g., the
#'   `-I` path to the package's `inst/include` directory).
#'
#' @export
#' @seealso [ld_flags()]
#' @examples
#' if (interactive()) {
#'   c_flags()
#' }
c_flags <- function() {

  # Find the directory /path/to/R/library/hdf5lib/include
  include_dir <- system.file("include", package = "hdf5lib")
  
  # Ensure the directory exists
  if (include_dir == "" || !dir.exists(include_dir))
    stop("C flags not found: The 'inst/include' directory is missing from hdf5lib.")
  
  # Return the compiler flag# Use normalizePath and winslash for robust paths
  paste0("-I", normalizePath(include_dir, winslash = "/", mustWork = TRUE))
}


#' Get C/C++ Linker Flags for hdf5lib
#'
#' @description
#' Provides the required linker flags to link against the static HDF5
#' library (`libhdf5.a`) bundled with the `hdf5lib` package.
#'
#' This function is intended to be called from a `Makevars` file by
#' other R packages that link to `hdf5lib`. It returns the `-L` path
#' to the library directory and the `-l` flags for `libhdf5` and
#' its system dependencies (like `pthread` and `dl`).
#'
#' @return A scalar character vector containing the linker flags.
#'
#' @export
#' @seealso [c_flags()]
#' @examples
#' if (interactive()) {
#'   ld_flags()
#' }
ld_flags <- function() {

  # Find the directory /path/to/R/library/hdf5lib/lib
  lib_dir <- system.file("lib", package = "hdf5lib")
  if (lib_dir == "" || !file.exists(lib_dir))
    stop("Linker flags not found: The 'inst/lib' directory is missing from hdf5lib.")
  
  # Ensure the library file actually exists in that directory
  static_lib_file <- file.path(lib_dir, "libhdf5.a")
  if (!file.exists(static_lib_file))
    stop("Linker flags not found: 'lib/libhdf5.a' is missing from hdf5lib.")
  
  # Create the -L flag pointing to the directory
  # Use normalizePath and winslash for robust paths
  lib_path_flag <- paste0("-L", normalizePath(lib_dir, winslash = "/", mustWork = TRUE))
  
  # Create a vector of all flags.
  # This correctly handles the case where the 'if' returns NULL.
  flags <- c(
    lib_path_flag, # The -L path to the library directory
    "-lhdf5",      # The -l name of the library
    "-lpthread",   # HDF5 dependency
    if (.Platform$OS.type == "unix") "-ldl" # HDF5 dependency on Unix
  )
  
  # Collapse all flags into a single, space-separated string
  paste(flags, collapse = " ")
}
