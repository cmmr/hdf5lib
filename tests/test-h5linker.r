# This script runs R CMD check on the minimal h5linker package
# located in inst/h5linker to perform an integration test.

message("Starting h5linker integration test (running R CMD build and check)...")

# prevent error `cannot open file 'startup.Rs': No such file or directory`
Sys.setenv("R_TESTS" = "")

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

# 4. Store current working dir and change to the temporary one
old_dir <- getwd()
setwd(work_dir)
# Ensure we change back to the original directory on exit
on.exit(setwd(old_dir), add = TRUE)

# 5. Build the tarball
message("Building h5linker tarball...")
# We must use the absolute path to the source directory
build_args <- c(
  "CMD", "build",
  shQuote(h5linker_src_dir)
)

# Run the build command
build_output <- system2(
  R_EXE,
  args = build_args,
  stdout = TRUE,
  stderr = TRUE
)

# 6. Find the built tarball
# We are inside work_dir, so we can use "."
tarball_name <- list.files(".", pattern = "\\.tar\\.gz$")
if (length(tarball_name) == 0) {
  message("--- R CMD build output ---")
  message(paste(build_output, collapse = "\n"))
  stop("R CMD build failed to create a tarball.")
}
# Get the absolute path for the check command
tarball_path <- file.path(work_dir, tarball_name[1])
message("Successfully built tarball: ", tarball_path)

# 7. Construct the R CMD check command on the tarball
cmd_args <- c(
  "CMD", "check",
  "--no-manual",
  "--as-cran",
  shQuote(tarball_path) # Use absolute path
)

message("Running command:")
message(paste(shQuote(R_EXE), paste(cmd_args, collapse = " ")))

# 8. Run R CMD check (no 'wd' argument)
# The h5linker.Rcheck directory will be created here, inside work_dir.
check_output <- system2(
  R_EXE,
  args = cmd_args,
  stdout = TRUE,
  stderr = TRUE
)

# 9. Check the exit status
check_status <- attr(check_output, "status")
exit_code <- if (is.null(check_status)) 0 else check_status

message("--- R CMD check output for h5linker ---")
message(paste(check_output, collapse = "\n"))
message("--- End h5linker output ---")

# 10. Change directory back before the final cleanup
# This is handled by the on.exit() hooks

if (exit_code != 0) {
  stop("R CMD check on h5linker failed with exit code: ", exit_code)
}

message("R CMD check on h5linker completed successfully!")
message("Test passed!")
