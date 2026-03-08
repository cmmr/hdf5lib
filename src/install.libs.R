# src/install.libs.R

# 1. Determine the correct architecture-specific destination
dest <- file.path(R_PACKAGE_DIR, paste0("libs", R_ARCH))
dir.create(dest, recursive = TRUE, showWarnings = FALSE)

# 2. Find libhdf5z.a library staged in inst/lib
staged_file <- file.path("..", "inst", "lib", "libhdf5z.a")

# 3. Move it to the official libs/ directory
if (file.exists(staged_file)) {
  file.copy(staged_file, dest, overwrite = TRUE)
  message("Successfully installed HDF5 static library to: ", dest)
} else {
  warning("No HDF5 static library found in the staging directory!")
}

# 4. Remove the staging directory
unlink(file.path("..", "inst", "lib"), recursive = TRUE)
