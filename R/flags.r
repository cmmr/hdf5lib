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
  
  # Return the compiler flag
  # Use normalizePath and winslash for robust paths
  paste0("-I", normalizePath(include_dir, winslash = "/", mustWork = TRUE))
}


#' Get C/C++ Linker Flags for hdf5lib
#'
#' @description
#' Provides the required linker flags to link against the dynamic HDF5
#' library (`libhdf5.so` or `libhdf5.dll`) bundled with the `hdf5lib` package.
#'
#' This function is intended to be called from a `Makevars` file by
#' other R packages that link to `hdf5lib`. It returns the `-L` path
#' to the library directory and the `-l` flag for `libhdf5`.
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

  # Find the package's 'libs' directory (e.g., /path/to/R/library/hdf5lib/libs)
  lib_dir_base <- system.file("libs", package = "hdf5lib")

  # Account for architecture-specific subdirectories (e.g., /libs/x64)
  lib_dir_arch <- file.path(lib_dir_base, .Platform$r_arch)

  # Use the arch-specific path if it exists, otherwise use the base 'libs' path
  if (dir.exists(lib_dir_arch)) {
    final_lib_dir <- lib_dir_arch
  } else {
    final_lib_dir <- lib_dir_base
  }

  if (final_lib_dir == "" || !dir.exists(final_lib_dir))
    stop("Linker flags not found: The 'libs' directory is missing from hdf5lib.")
  
  # Ensure the shared library file actually exists in that directory
  # .Platform$dynlib.ext gives ".so", ".dll", or ".dylib"
  shlib_file <- file.path(final_lib_dir, paste0("libhdf5", .Platform$dynlib.ext))
  
  if (!file.exists(shlib_file))
    stop(paste("Linker flags not found:", shlib_file, "is missing from hdf5lib."))
  
  # Create the -L flag pointing to the directory
  # Use normalizePath and winslash for robust paths
  lib_path_flag <- paste0("-L", normalizePath(final_lib_dir, winslash = "/", mustWork = TRUE))

  # Create a vector of all flags.
  # We only need -L and -l. The dynamic linker will handle
  # transitive dependencies (like pthread, dl) from libhdf5.so
  flags <- c(
    lib_path_flag, # The -L path to the library directory
    "-lhdf5"       # The -l name of the library
  )
  
  # Collapse all flags into a single, space-separated string
  paste(flags, collapse = " ")
}