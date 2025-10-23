# Test script run by R CMD check
# Loads the package and calls the internal C function

# Check if the package namespace can be loaded
if (!requireNamespace("hdf5lib", quietly = TRUE)) {
  stop("Failed to load hdf5lib namespace.")
}

# Call the internal function (defined in R/smoke_test.r)
# which in turn calls the C function via .Call()
message("Running internal C smoke test...")
version_string <- tryCatch({
  # Access internal function using :::
  hdf5lib:::smoke_test()
}, error = function(e) {
  stop("Error calling internal C function C_smoke_test(): ", e$message)
})

# Check the result
expected_version <- sub('\\.\\d+$', '', packageVersion('hdf5lib'))
if (is.null(version_string)) {
  stop("Internal C function returned NULL.")
} else if (!is.character(version_string) || length(version_string) != 1) {
  stop("Internal C function returned unexpected type or length.")
} else if (version_string != expected_version) {
  stop(paste("Internal C function returned incorrect version. Expected:",
              expected_version, "Got:", version_string))
} else {
  message("Internal C smoke test passed. HDF5 version: ", version_string)
}

# If we reach here without stopping, the test passed.
# R CMD check looks for non-zero exit status or calls to stop() to indicate failure.