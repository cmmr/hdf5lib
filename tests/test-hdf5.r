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

  # The version macros in hdf5lib.h match DESCRIPTION
  hdr_version <- .Call("C_hdf5lib_version")
  pkg_version <- packageVersion("hdf5lib")
  pkg_parts   <- unlist(pkg_version)

  if (!identical(hdr_version[[1]], as.character(pkg_version)))
    stop("HDF5LIB_VERSION_STRING is ", hdr_version[[1]], ", but DESCRIPTION has ", pkg_version)

  if (!identical(hdr_version[[2]][1:4], pkg_parts))
    stop("HDF5LIB_VERSION_{MAJOR,MINOR,PATCH,REVISION} are ",
         paste(hdr_version[[2]][1:4], collapse = "."), ", but DESCRIPTION has ", pkg_version)

  if (hdr_version[[2]][5] != sum(pkg_parts * c(1000000L, 10000L, 100L, 1L)))
    stop("HDF5LIB_VERSION is ", hdr_version[[2]][5], ", which does not encode ", pkg_version)

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
