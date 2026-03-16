library(hdf5lib)

interop_dir <- normalizePath(".", mustWork = TRUE)
r_exe <- normalizePath(file.path(R.home("bin"), "R"), mustWork = FALSE)

# --- File Definitions ---
c_reader      <- file.path(interop_dir, "verify_python_zoo.c")
so_reader     <- file.path(interop_dir, paste0("verify_python_zoo", .Platform$dynlib.ext))
python_h5     <- file.path(interop_dir, "encoding_zoo.h5")

c_writer      <- file.path(interop_dir, "generate_c_zoo.c")
so_writer     <- file.path(interop_dir, paste0("generate_c_zoo", .Platform$dynlib.ext))
c_h5          <- file.path(interop_dir, "c_encoding_zoo.h5")

# --- Setup Environment ---
Sys.setenv(
  PKG_CPPFLAGS = c_flags(),
  PKG_LIBS     = ld_flags()
)

on.exit({
  Sys.unsetenv(c("PKG_CPPFLAGS", "PKG_LIBS"))
  if (is.loaded("C_read_zoo")) dyn.unload(so_reader)
  if (is.loaded("C_write_zoo")) dyn.unload(so_writer)
  
  # Clean up compiled artifacts
  files_to_clean <- c(
      file.path(interop_dir, "verify_python_zoo.o"), so_reader,
      file.path(interop_dir, "generate_c_zoo.o"), so_writer
  )
  for (f in files_to_clean) {
      if (file.exists(f)) file.remove(f)
  }
}, add = TRUE)

# ==========================================
# PHASE 1: Verify Python -> C
# ==========================================
if (!file.exists(python_h5)) {
  stop("Required Python HDF5 file missing: ", python_h5)
}

message("\n=== Compiling C Reader Module ===")
status_read <- system(sprintf('%s CMD SHLIB %s', shQuote(r_exe), shQuote(c_reader)))
if (status_read != 0) stop("Could not compile C reader.")

dyn.load(so_reader)
message("\n=== Executing Python -> C Verification ===")
result_read <- tryCatch({
  .Call("C_read_zoo", python_h5)
}, error = function(e) stop("Validation failed during C read execution: ", e$message))

if (!identical(as.integer(result_read), 1L)) stop("Unexpected result from C reader.")
message("SUCCESS: Python datasets natively read by C.")

# ==========================================
# PHASE 2: Generate C -> Python
# ==========================================
message("\n=== Compiling C Writer Module ===")
status_write <- system(sprintf('%s CMD SHLIB %s', shQuote(r_exe), shQuote(c_writer)))
if (status_write != 0) stop("Could not compile C writer.")

dyn.load(so_writer)
message("\n=== Executing C -> Python Generation ===")
result_write <- tryCatch({
  .Call("C_write_zoo", c_h5)
}, error = function(e) stop("Generation failed during C write execution: ", e$message))

if (!identical(as.integer(result_write), 1L)) stop("Unexpected result from C writer.")
if (!file.exists(c_h5)) stop("C Writer did not produce output file: ", c_h5)

message("SUCCESS: C datasets generated successfully. Ready for Python verification.")
