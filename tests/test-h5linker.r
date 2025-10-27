# This script runs R CMD check on the minimal h5linker package
# located in inst/h5linker to perform an integration test.

message("Starting h5linker integration test (running R CMD check)...")

# 1. Find the h5linker source directory within the installed hdf5lib
h5linker_src_dir <- system.file("h5linker", package = "hdf5lib")
if (!dir.exists(h5linker_src_dir) || h5linker_src_dir == "") {
  stop("Could not find the h5linker source directory within hdf5lib installation.")
}
message("Found h5linker source at: ", h5linker_src_dir)

# 2. Find the R executable
R_EXE <- file.path(R.home("bin"), "R")

# 3. Construct the R CMD check command
#    Use --no-manual --as-cran for a standard check
#    Output redirection might be needed depending on CI setup,
#    but system2 captures stdout/stderr.
cmd_args <- c(
  "CMD", "check",
  "--no-manual",
  "--as-cran",
  shQuote(h5linker_src_dir) 
)

message("Running command:")
message(paste(shQuote(R_EXE), paste(cmd_args, collapse=" ")))

# 4. Run R CMD check using system2
check_output <- system2(
  R_EXE,
  args = cmd_args,
  stdout = TRUE,
  stderr = TRUE
)

# 5. Check the exit status
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
