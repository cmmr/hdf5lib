local({
  
  tmp_dir <- tempfile("hdf5lib_smoke_")
  dir.create(tmp_dir)
  file.copy("smoke_test.c", file.path(tmp_dir, "smoke_test.c"))
  file.copy("Makevars",     file.path(tmp_dir, "Makevars"))
  old_wd  <- setwd(tmp_dir)
  so_file <- paste0("smoke_test", .Platform$dynlib.ext)
  tmp_h5  <- tempfile(fileext = ".h5")
  
  old_r_tests <- Sys.getenv("R_TESTS")
  Sys.setenv(R_TESTS = "")
  
  on.exit({
    if (so_file %in% names(getLoadedDLLs())) {
      dyn.unload(so_file)
      gc()
      Sys.sleep(0.2)
    }
    setwd(old_wd)
    Sys.setenv(R_TESTS = old_r_tests)
    try(silent = TRUE, unlink(tmp_dir, recursive = TRUE))
  }, add = TRUE)
  
  # Compile `smoke_test.c`: system() returns 0 on success.
  compile_cmd <- paste(shQuote(file.path(R.home("bin"), "R")), 'CMD SHLIB smoke_test.c')
  exit_status <- system(compile_cmd)
  
  if (exit_status != 0)
    stop("R CMD SHLIB failed to compile smoke_test.c")
  
  if (!file.exists(so_file))
    stop(paste("Shared object missing at:", so_file))
  
  
  # Load the shared object
  dyn.load(so_file)
  
  # Call the C function (from smoke_test.c)
  version_str <- .Call("C_smoke_test", tmp_h5)
  
  # Verify results
  if (!inherits(version_str, "character"))
    stop("version_str is not a character")
  
  if (!file.exists(tmp_h5))
    stop("HDF5 file was not created by C code.")
  
  if (!grepl("^[0-9]+\\.[0-9]+\\.[0-9]+$", version_str))
    stop(paste("version_str did not match the expected pattern. Got:", version_str))
  
})
