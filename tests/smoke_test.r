# This script is run by R CMD check to "smoke test" the hdf5lib
# linking functionality.

# 1. Setup paths
message("Starting hdf5lib link test...")
if (!requireNamespace("hdf5lib", quietly = TRUE)) {
  stop("Failed to load hdf5lib namespace. Cannot run link test.")
}

test_c_file <- system.file("tests", "smoke_test.c", package = "hdf5lib")
if (!file.exists(test_c_file)) {
  stop("Could not find test C file at: ", test_c_file)
}

# Define the *full* output filename
test_lib_base <- file.path(tempdir(), "smoke_test_lib")
lib_file_to_create <- paste0(test_lib_base, .Platform$dynlib.ext)

# 2. Get the build flags from hdf5lib's R functions
message("Retrieving build flags from hdf5lib R API...")
cflags <- hdf5lib::c_flags()
libs <- hdf5lib::ld_flags()

# 3. Build and run the command using system()
R_EXE <- file.path(R.home("bin"), "R")

# Use paste0() to join the variable name directly to its quoted value
env_var_1 <- paste0("PKG_CPPFLAGS=", shQuote(cflags))
env_var_2 <- paste0("PKG_LIBS=", shQuote(libs))

# Build the main R command part
r_cmd <- paste(
  shQuote(R_EXE),
  "CMD SHLIB",
  shQuote(test_c_file),
  "-o", shQuote(lib_file_to_create) # <-- Use the full filename
)

# Combine the variable assignments and the command
full_cmd <- paste(env_var_1, env_var_2, r_cmd)

message("Compiling test C code with command:")
message(full_cmd)

# Run the compilation. system() will return 0 for success.
compile_status <- system(full_cmd)

# Check for a non-zero exit status
if (compile_status != 0) {
  stop("R CMD SHLIB failed. Compilation returned non-zero exit status.")
}

# 4. Load and run the compiled function
#    Check for the *exact* file we told R CMD SHLIB to create.
if (!file.exists(lib_file_to_create)) {
  stop("Test library compilation failed. Output file not found.")
}

message("Compilation successful. Loading shared library...")
dyn.load(lib_file_to_create)

tmp_file <- tempfile(fileext = ".h5")
version_str <- NULL
tryCatch({
  version_str <- .Call("C_smoke_test", tmp_file)
}, error = function(e) {
  stop("Error during .Call('C_smoke_test'): ", e$message)
})

# 5. Check results
message("C function executed. Checking results...")
if (!file.exists(tmp_file)) {
  stop("Test failed: C function did not create the output file.")
}

if (is.null(version_str) || !grepl("^[0-9]+\\.[0-9]+\\.[0-9]+$", version_str)) {
  stop("Test failed: C function did not return a valid version string.")
}

message("HDF5 C API call successful. Reported version: ", version_str)
message("Test passed!")

# 6. Clean up
file.remove(tmp_file)
dyn.unload(lib_file_to_create)