# Test script run by R CMD check
# Loads the package and calls the internal C function

# Check if the package namespace can be loaded
if (!requireNamespace("hdf5lib", quietly = TRUE)) {
  stop("Failed to load hdf5lib namespace.")
}


test_lib <- file.path(tempdir(), "test_smoke")

# Get compile flags from hdf5lib's Makevars
makevars_file <- system.file("include", "Makevars", package = "hdf5lib")
makevars_lines <- readLines(makevars_file)

# Extract PKG_LIBS (with R_PACKAGE_DIR expanded)
pkg_dir <- system.file(package = "hdf5lib")
libs <- gsub("\\$\\(R_PACKAGE_DIR\\)", pkg_dir, makevars_lines)

# Get C flags
cppflags <- paste0("-I", system.file("include", package = "hdf5lib"))

# Compile!
system(paste(
  "R CMD SHLIB",
  system.file("tests", "src", "smoke_test.c", package = "hdf5lib"),
  "-o", test_lib,
  "-e", shQuote(cppflags),
  "-e", shQuote(libs)
))

# 2. Load and run the compiled function
dyn.load(paste0(test_lib, .Platform$dynlib.ext))

tmp_file <- tempfile(fileext = ".h5")
version_str <- .Call("C_smoke_test", tmp_file)

# 3. Check the result
expect_true(file.exists(tmp_file))
expect_match(version_str, "^[0-9]+\\.[0-9]+\\.[0-9]+$")

# 4. Clean up
file.remove(tmp_file)


# Call the internal function (defined in tests/src/smoke_test.r)
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