#' Internal Function to Call C Version Test
#'
#' This function calls the internal C routine \code{C_hdf5_version}
#' which performs several HDF5 operations (get version, write with HL,
#' read with LL) using a temporary file. This is primarily used for
#' testing the successful linkage and basic functionality of the bundled
#' HDF5 library.
#'
#' @return A character string containing the HDF5 library version detected by
#'   the C code, e.g., "1.14.6". Returns `NULL` if the C call fails.
#' @keywords internal
#' @noRd
hdf5_version <- function() {

  # Create a temporary file path
  tmp_h5_file <- tempfile(fileext = ".h5")
  # Ensure cleanup even if function errors
  on.exit(unlink(tmp_h5_file, force = TRUE), add = TRUE)

  # Call the C function registered as "C_hdf5_version"
  result <- tryCatch({
    # .Call returns a SEXP, which should be a character vector
    result <- .Call("C_hdf5_version", tmp_h5_file, PACKAGE = "hdf5lib")
  }, error = function(e) {
    stop("Internal C function 'C_hdf5_version' failed: ", e$message)
  })

  # Validate and return the result
  if (is.character(result) && length(result) == 1) {
    return(result)
  } else {
    stop("Internal C function 'C_hdf5_version' returned unexpected type or length.")
  }
}