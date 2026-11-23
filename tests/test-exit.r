# On Windows, R used to hang at exit once the Blosc or Blosc2 filter had used
# more than one thread: HDF5 shuts down after Windows has killed the worker
# threads, and freeing them waited on those threads forever.  Run that in a
# child R process, which must exit, and check what HDF5 wrote on its way out.
# It runs only where NOT_CRAN is true -- the GitHub workflows and
# devtools::check() -- and not on CRAN, since it may have to kill that process.
if (.Platform$OS.type == "windows" && isTRUE(as.logical(Sys.getenv("NOT_CRAN", "false")))) local({

  tmp_dir <- normalizePath(tempfile("hdf5lib_exit_"), winslash = "/", mustWork = FALSE)
  dir.create(tmp_dir)
  file.copy("exit_test.c", file.path(tmp_dir, "exit_test.c"))
  file.copy("Makevars",    file.path(tmp_dir, "Makevars"))
  old_wd <- setwd(tmp_dir)

  old_r_tests <- Sys.getenv("R_TESTS")
  Sys.setenv(R_TESTS = "")

  on.exit({
    setwd(old_wd)
    Sys.setenv(R_TESTS = old_r_tests)
  }, add = TRUE)

  compile_cmd <- paste(shQuote(file.path(R.home("bin"), "R")), 'CMD SHLIB exit_test.c')
  if (system(compile_cmd) != 0)
    stop("R CMD SHLIB failed to compile exit_test.c")

  so_file   <- file.path(tmp_dir, "exit_test.dll")
  h5_file   <- file.path(tmp_dir, "exit.h5")
  pid_file  <- file.path(tmp_dir, "child.pid")
  done_file <- file.path(tmp_dir, "child.done")
  log_file  <- file.path(tmp_dir, "child.log")

  writeLines(c(
    sprintf("writeLines(as.character(Sys.getpid()), %s)", deparse(pid_file)),
    sprintf("dyn.load(%s)", deparse(so_file)),
    sprintf("invisible(.Call('C_exit_test_write', %s))", deparse(h5_file)),
    sprintf("invisible(file.create(%s))", deparse(done_file))),
    "child.R")

  system2(file.path(R.home("bin"), "Rscript.exe"), c("--vanilla", "child.R"),
          stdout = log_file, stderr = log_file, wait = FALSE)

  child_pid <- function() {
    if (file.exists(pid_file)) suppressWarnings(as.integer(readLines(pid_file)[1])) else NA
  }
  is_running <- function(pid) {
    out <- system2("tasklist", c("/FI", shQuote(paste("PID eq", pid)), "/FO", "CSV", "/NH"),
                   stdout = TRUE, stderr = FALSE)
    any(grepl(sprintf('"%d"', pid), out, fixed = TRUE))
  }
  child_log <- function() paste(if (file.exists(log_file)) readLines(log_file), collapse = "\n")

  # Wait for the child to finish its script, or to fail
  deadline <- Sys.time() + 120
  repeat {
    pid    <- child_pid()
    exited <- !is.na(pid) && !is_running(pid)
    if (file.exists(done_file)) break
    if (exited || Sys.time() > deadline)
      stop("The child R process did not finish its script:\n", child_log())
    Sys.sleep(0.5)
  }

  # Then for it to exit.  Kill the whole process tree if it does not, so that
  # nothing is left holding exit_test.dll.
  pid      <- child_pid()
  deadline <- Sys.time() + 30
  while (is_running(pid) && Sys.time() < deadline) Sys.sleep(0.5)
  if (is_running(pid)) {
    system2("taskkill", c("/F", "/T", "/PID", pid), stdout = FALSE, stderr = FALSE)
    stop("R did not exit within 30 seconds of finishing a script that used ",
         "two threads in the Blosc and Blosc2 filters")
  }

  # The datasets left open were compressed as the child exited
  dyn.load(so_file)
  ok <- .Call("C_exit_test_verify", h5_file)
  names(ok) <- c("/b1", "/b2", "/b1_open", "/b2_open")
  if (!all(ok))
    stop("Not read back intact after the child exited: ", paste(names(ok)[!ok], collapse = ", "))
})
