# This script is run by R CMD check to "smoke test" the hdf5lib
# linking functionality.

# 1. Setup paths
message("Starting hdf5lib link test...")
if (!requireNamespace("hdf5lib", quietly = TRUE)) {
  stop("Failed to load hdf5lib namespace. Cannot run link test.")
}

# Normalize paths to use R-friendly forward slashes
test_c_file <- normalizePath(
  system.file("tests", "smoke_test.c", package = "hdf5lib"),
  winslash = "/", 
  mustWork = TRUE
)

test_lib_base <- normalizePath(
  file.path(tempdir(), "smoke_test_lib"),
  winslash = "/",
  mustWork = FALSE
)
lib_file_to_create <- paste0(test_lib_base, .Platform$dynlib.ext)


# 2. Get the build flags from hdf5lib's R functions
message("Retrieving build flags from hdf5lib R API...")
cflags <- hdf5lib::c_flags()
libs <- hdf5lib::ld_flags()

# 3. Set environment variables for the child process
#    This is the core of this approach. R CMD SHLIB will
#    inherit these variables from the R process.
Sys.setenv(
  PKG_CPPFLAGS = cflags,
  PKG_LIBS = libs
)

# 4. Build and run the command using system2()
R_EXE <- file.path(R.home("bin"), "R")

# Arguments are simple and unquoted
cmd_args <- c(
  "CMD", "SHLIB",
  test_c_file,
  "-o", lib_file_to_create
)

message("Compiling test C code with command:")
# We log the equivalent shell command for debugging
message(paste(
  paste0("PKG_CPPFLAGS=", shQuote(cflags)),
  paste0("PKG_LIBS=", shQuote(libs)),
  shQuote(R_EXE),
  paste(cmd_args, collapse = " "),
  sep = " "
))

# Run the compilation
# No 'env' or 'system()' logic needed.
compile_output <- system2(
  R_EXE,
  args = cmd_args,
  stdout = TRUE,
  stderr = TRUE
)

# Check for a non-zero exit status
compile_status <- attr(compile_output, "status")
if (!is.null(compile_status) && compile_status != 0) {
  message("--- COMPILER OUTPUT ---")
  message(paste(compile_output, collapse = "\n"))
  message("--- END COMPILER OUTPUT ---")
  stop("R CMD SHLIB failed. Compilation returned non-zero exit status.")
}

# Print output on success
message(paste(compile_output, collapse = "\n"))

# 5. Load and run the compiled function
if (!file.exists(lib_file_to_create)) {
  stop("Test library compilation failed. Output file not found.")
}

message("Compilation successful. Loading shared library...")
dyn.load(lib_file_to_create)

tmp_file <- normalizePath(
  tempfile(fileext = ".h5"),
  winslash = "/",
  mustWork = FALSE
)
version_str <- NULL
tryCatch({
  version_str <- .Call("C_smoke_test", tmp_file)
}, error = function(e) {
  stop("Error during .Call('C_smoke_test'): ", e$message)
})

# 6. Check results
message("C function executed. Checking results...")
if (!file.exists(tmp_file)) {
  stop("Test failed: C function did not create the output file.")
}

if (is.null(version_str) || !grepl("^[0-9]+\\.[0-9]+\\.[0-9]+$", version_str)) {
  stop("Test failed: C function did not return a valid version string.")
}

message("HDF5 C API call successful. Reported version: ", version_str)
message("Test passed!")

# 7. Clean up
file.remove(tmp_file)
dyn.unload(lib_file_to_create)