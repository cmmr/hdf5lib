# Load the package to access c_flags() and ld_flags()
library(hdf5lib)

interop_dir <- normalizePath(".", mustWork = TRUE)
c_file      <- file.path(interop_dir, "read_zoo.c")
o_file      <- file.path(interop_dir, "read_zoo.o")
so_file     <- file.path(interop_dir, paste0("read_zoo", .Platform$dynlib.ext))
h5_file     <- file.path(interop_dir, "encoding_zoo.h5")

if (!file.exists(h5_file)) {
  stop("Required HDF5 file missing: ", h5_file)
}

# Set environment variables using hdf5lib's exported configuration
Sys.setenv(
  PKG_CPPFLAGS = c_flags(),
  PKG_LIBS     = ld_flags()
)

# Ensure environment variables and compiled artifacts are cleaned up on exit
on.exit({
  Sys.unsetenv(c("PKG_CPPFLAGS", "PKG_LIBS"))
  if (is.loaded("C_read_zoo")) dyn.unload(so_file)
  if (file.exists(o_file)) file.remove(o_file)
  if (file.exists(so_file)) file.remove(so_file)
}, add = TRUE)

R_EXE <- normalizePath(file.path(R.home("bin"), "R"), mustWork = FALSE)
compile_cmd <- sprintf('%s CMD SHLIB %s', shQuote(R_EXE), shQuote(c_file))

message("Compiling interoperability module:\n", compile_cmd)

# Execute the compilation
status <- system(compile_cmd)
if (status != 0) {
  stop("Could not compile interop module. Exit status: ", status)
}

if (!file.exists(so_file)) {
  stop("Shared object file not created: ", so_file)
}

# Load and execute the C routine
dyn.load(so_file)
message("\nExecuting C_read_zoo...")

result <- tryCatch({
  .Call("C_read_zoo", h5_file)
}, error = function(e) {
  stop("Interop validation failed during C execution: ", e$message)
})

# Verify the C routine returned the expected success scalar
if (identical(as.integer(result), 1L)) {
  message("\nSUCCESS: Cross-language dataset validation completed without errors.")
} else {
  stop("Interop validation returned an unexpected result code: ", result)
}
