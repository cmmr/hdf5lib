
test_that("HDF5 smoke test", {
  
  c_file  <- test_path("../src/smoke_test.c")
  o_file  <- test_path("../src/smoke_test.o")
  so_file <- paste0(test_path("../src/smoke_test"), .Platform$dynlib.ext)
  
  compile_cmd <- sprintf(
    '%s CMD SHLIB %s %s %s',
    shQuote(normalizePath(Sys.which('R'))),
    shQuote(c_file),
    c_flags(),
    ld_flags() )
  
  tryCatch(
    expr  = {
      system(compile_cmd)
      succeed("Compiled successfully.")
    }, 
    error = function (e) {
      fail(paste("Could not compile with command:\n", compile_cmd, "\n", e$message))
    })
  
  if (file.exists(so_file)) {
    succeed("Shared object created successfully.")
    on.exit(file.remove(o_file, so_file), add = TRUE)
  } else {
    fail("Compilation failed. Shared object file not found.")
  }
  
  expect_silent(dyn.load(so_file))
  
  
  # Call C the function
  tryCatch({
    tmp_file    <- tempfile(fileext = ".h5")
    tmp_file    <- normalizePath(tmp_file, winslash = "/", mustWork = FALSE)
    version_str <- .Call("C_smoke_test", tmp_file)
    succeed(".Call('C_smoke_test') ran successfully")
  }, error = function(e) {
    fail(paste("Error during .Call('C_smoke_test'):", e$message))
  })
  
  if (file.exists(tmp_file)) {
    succeed("Output H5 file created successfully.")
    on.exit(file.remove(tmp_file), add = TRUE)
  } else {
    fail("Test failed: C function did not create the H5 output file.")
  }
  
  expect_vector(version_str, character(0), 1)
  expect_match(version_str, "^[0-9]+\\.[0-9]+\\.[0-9]+$")
})
