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

# 3. Build and run the command (platform-specific)
R_EXE <- file.path(R.home("bin"), "R")

# Build the main R command part
r_cmd <- paste(
  shQuote(R_EXE),
  "CMD SHLIB",
  shQuote(test_c_file),
  "-o", shQuote(lib_file_to_create)
)

# Construct the command string differently for Windows vs. Unix.
if (.Platform$OS.type == "windows") {
  # --- On Windows, use "set VAR=VAL && command" syntax ---
  # Note: shQuote() on Windows uses double quotes (""), which is correct.
  env_var_1 <- paste0("set PKG_CPPFLAGS=", shQuote(cflags))
  env_var_2 <- paste0("set PKG_LIBS=", shQuote(libs))
  
  # Combine with '&&' and redirect stderr
  full_cmd <- paste(env_var_1, "&&", env_var_2, "&&", r_cmd, "2>&1")
  
} else {
  # --- On Unix (macOS/Linux), use "VAR='VAL' command" syntax ---
  # Note: shQuote() on Unix uses single quotes (''), which is correct.
  
  # Use paste0() to join var name to its quoted value
  env_var_1 <- paste0("PKG_CPPFLAGS=", shQuote(cflags))
  env_var_2 <- paste0("PKG_LIBS=", shQuote(libs))
  
  # Combine variables, command, and redirect stderr
  full_cmd <- paste(env_var_1, env_var_2, r_cmd, "2>&1")
}

message("Compiling test C code with command:")
message(full_cmd)

# Run and capture all output
compile_output <- system(full_cmd, intern = TRUE)
compile_status <- attr(compile_output, "status")
if (is.null(compile_status)) {
  compile_status <- 0 # No status attribute means success
}

# Check for a non-zero exit status
if (compile_status != 0) {
  message("--- COMPILER OUTPUT ---")
  message(paste(compile_output, collapse = "\n"))
  message("--- END COMPILER OUTPUT ---")
  stop("R CMD SHLIB failed. Compilation returned non-zero exit status.")
}

# Print output on success
message(paste(compile_output, collapse = "\n"))

# 4. Load and run the compiled function
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