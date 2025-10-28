# This script runs R CMD check on the minimal h5linker package
# located in inst/h5linker to perform an integration test.

message("Starting h5linker integration test (running R CMD build and check)...")

# 1. Find the h5linker source directory within the installed hdf5lib
h5linker_src_dir <- system.file("h5linker", package = "hdf5lib")
if (!dir.exists(h5linker_src_dir) || h5linker_src_dir == "") {
  stop("Could not find the h5linker source directory within hdf5lib installation.")
}
message("Found h5linker source at: ", h5linker_src_dir)

# 2. Create a temporary working directory for all build/check artifacts
work_dir <- tempfile(pattern = "h5linker-test-")
dir.create(work_dir)
message("Created temporary working directory at: ", work_dir)

# Ensure cleanup of the entire working directory on exit
on.exit(unlink(work_dir, recursive = TRUE, force = TRUE), add = TRUE)

# 3. Find the R executable
R_EXE <- file.path(R.home("bin"), "R")

# 4. Build the tarball
message("Building h5linker tarball...")
build_args <- c(
  "CMD", "build",
  "--no-manual",
  "--no-vignettes",
  shQuote(h5linker_src_dir)
)

# Run the build command *inside* the working directory
build_output <- system2(
  R_EXE,
  args = build_args,
  stdout = TRUE,
  stderr = TRUE,
  wd = work_dir
)

# 5. Find the built tarball
tarball_name <- list.files(work_dir, pattern = "\\.tar\\.gz$")
if (length(tarball_name) == 0) {
  message("--- R CMD build output ---")
  message(paste(build_output, collapse = "\n"))
  stop("R CMD build failed to create a tarball.")
}
tarball_path <- file.path(work_dir, tarball_name[1])
message("Successfully built tarball: ", tarball_path)

# 6. Construct the R CMD check command on the tarball
cmd_args <- c(
  "CMD", "check",
  "--no-manual",
  "--as-cran",
  shQuote(tarball_path)
)

message("Running command:")
message(paste(shQuote(R_EXE), paste(cmd_args, collapse = " ")))

# 7. Run R CMD check
# We also run this in the temp directory, so the
# h5linker.Rcheck directory is created there.
check_output <- system2(
  R_EXE,
  args = cmd_args,
  stdout = TRUE,
  stderr = TRUE,
  wd = work_dir
)

# 8. Check the exit status
check_status <- attr(check_output, "status")
exit_code <- if (is.null(check_status)) 0 else check_status

message("--- R CMD check output for h5linker ---")
message(paste(check_output, collapse = "\n"))
message("--- End h5linker output ---")

if (exit_code != 0) {
  stop("R CMD check on h5linker failed with exit code: ", exit_code)
}

message("R CMD check on h5linker completed successfully!")
message("Test passed!")
