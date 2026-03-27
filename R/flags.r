
validate_api <- function(api) {
  switch(
    EXPR = as.character(api), 'latest' = '200',
    '200' = '200', '2.00' = '200', '2.0' = '200', '2' = '200',
    '114' = '114', '1.14' = '114',
    '112' = '112', '1.12' = '112',
    '110' = '110', '1.10' = '110', '1.1' = '110',
    '18'  = '18',  '1.08' = '18',  '1.8' = '18',
    '16'  = '16',  '1.06' = '16',  '1.6' = '16',
    stop("Invalid API version specified: '", api, "'.") )
}


#' Get C/C++ Compiler Flags for hdf5lib
#'
#' @description
#' Provides the required C/C++ compiler flags to find the HDF5 header
#' files bundled with the `hdf5lib` package.
#' 
#' @param api A numeric value specifying the HDF5 API version to use (e.g.,
#'   `1.14` for v1.14), or the string `"latest"`. This adds a preprocessor
#'   directive like `-DH5_USE_114_API_DEFAULT` to ensure that the compiled
#'   code uses symbols compatible with a specific version of the HDF5 API.
#'   This is useful for maintaining compatibility with older HDF5 versions.
#'   Supported values are `2.0`, `1.14`, `1.12`, `1.10`, `1.8`, and `1.6`.
#'   Defaults to `"latest"`, which corresponds to the newest supported
#'   API version.
#'
#' @return A scalar character vector containing the compiler flags (e.g., the
#'   `-I` path to the package's `inst/include` directory).
#'
#' @export
#' @seealso [ld_flags()]
#' @examples
#' c_flags()
#' c_flags(api = 1.14)
#' 
c_flags <- function(api = "latest") {
  
  api <- validate_api(api)
  
  # Find the HDF5 header file (e.g., /path/to/R/library/hdf5lib/include/hdf5.h)
  hdf5_header <- list.files(
    path       = system.file("include", package = "hdf5lib"),
    pattern    = "hdf5\\.h",
    full.names = TRUE,
    recursive  = TRUE )
  
  if (length(hdf5_header) != 1)
    stop("Header file not found: 'include/hdf5.h' is missing from hdf5lib.")
  
  # Quote if the path contains spaces or other shell-special characters.
  # Don't quote by default, as that can sometimes cause other problems.
  include_dir <- normalizePath(dirname(hdf5_header), winslash = "/")
  if (grepl("[ &'();]", include_dir))
    include_dir <- shQuote(include_dir)
  
  # Return the compiler flag
  paste(
    paste0("-I", include_dir), 
    "-DH5_BUILT_AS_STATIC_LIB", 
    paste0("-DH5_USE_", api, "_API_DEFAULT"))
}


#' Get C/C++ Linker Flags for hdf5lib
#'
#' @description
#' Provides the required linker flags to link against the static HDF5
#' library (`libhdf5z.a`) bundled with the `hdf5lib` package.
#'
#' @param api A numeric value or the string `"latest"`. This parameter is
#'   included for consistency with [c_flags()] and is reserved for future use;
#'   it currently has no effect on the linker flags. Defaults to `"latest"`.
#'
#' @return A scalar character vector containing the linker flags.
#'
#' @export
#' @seealso [c_flags()]
#' @examples
#' ld_flags()
#' 
ld_flags <- function(api = "latest") {
  
  api <- validate_api(api)

  # Find the static library (e.g., /path/to/R/library/hdf5lib/lib/libhdf5z.a)
  # This corresponds to the 'inst/lib' directory in the configure script.
  static_lib <- list.files(
    path       = system.file("lib", package = "hdf5lib"),
    pattern    = "libhdf5z\\.a",
    full.names = TRUE,
    recursive  = TRUE )
  
  if (length(static_lib) != 1)
    stop("Static library not found: 'lib/libhdf5z.a' is missing from hdf5lib.")
  
  # Quote if the path contains spaces or other shell-special characters.
  # Don't quote by default, as that can sometimes cause other problems.
  lib_dir <- normalizePath(dirname(static_lib), winslash = "/")
  if (grepl("[ &'();]", lib_dir))
    lib_dir <- shQuote(lib_dir)

  # Create a vector of all flags.
  # The downstream package must now link to hdf5 and its dependencies.
  flags <- c(
    paste0("-L", lib_dir),
    "-lhdf5z",    # Link to our libhdf5z.a static library
    "-lpthread",  # HDF5 dependency for thread-safety
    if (.Platform$OS.type == "unix") "-ldl" else '-lws2_32'
  )
  
  # Append Exported Sanitizer Flags
  # We look for the file we created in configure
  flags_file <- system.file("exported_flags.txt", package = "hdf5lib")
  if (file.exists(flags_file))
    flags <- c(flags, trimws(readLines(flags_file, warn = FALSE)))

  # Collapse all flags into a single, space-separated string
  paste(flags, collapse = " ")
}
