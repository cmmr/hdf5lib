# Use this file to download, patch, and minify the latest version of HDF5.
# The output of this script should be saved to src/hdf5.tar.gz and bundled
# with the hdf5lib source package.

VER <- "1.14.6"


# Download and extract the HDF5 codebase
url     <- paste0("https://github.com/HDFGroup/hdf5/releases/download/hdf5_", VER, "/hdf5-", VER, ".tar.gz")
tarfile <- basename(url)
download.file(url, tarfile)

cat('Decompressing', tarfile, '...\n')
utils::untar(tarfile)
invisible(file.rename(paste0("hdf5-", VER), 'hdf5'))
setwd('hdf5')


# We've disabled a bunch of features, so no need to bundle that code.
# The Makefiles are expected by the configure script though, so keep them.
cat('Identifying unneeded files...\n')
to_remove <- list.files(
  full.names = TRUE,
  recursive  = TRUE,
  path       = c(
    "c++", "doxygen", "fortran", "HDF5Examples", "java",
    "m4", "release_docs", "test", "testpar", "tools", "utils",
    file.path("hl", c("c++", "examples", "fortran", "test", "tools"))
  ))
to_remove <- to_remove[!endsWith(to_remove, ".in")]
cat('Removing', length(to_remove), 'files...\n')
invisible(file.remove(to_remove))

cat('Deleting empty directories...\n')
for (d in rev(sort(list.dirs(full.names = TRUE, recursive = TRUE))))
  if (length(dir(d, include.dirs = TRUE)) == 0) unlink(d, force = TRUE)


# Here's where we apply surgical fixes and add new code.
cat('Applying hdf5-patches...\n')
patch_dir <- file.path('..', 'hdf5-patches')
for (patch_file in list.files(patch_dir, full.names = TRUE)) {
  cat("->", basename(patch_file), "\n")
  system2('patch', '-p0', stdin = patch_file)
}


# Global search and replace incompatible C functions.
c_files <- list.files(c("src", "hl"), "\\.c$", full.names = TRUE, recursive = TRUE)
cat('Converting', length(c_files), '.c files to R\'s C API...\n')
for (c_file in c_files) {
  code <- readChar(c_file, file.size(c_file))
  code <- gsub('\\bstdout\\b', 'Rstdout',  code) # stdout  -> Rstdout
  code <- gsub('\\bstderr\\b', 'Rstderr',  code) # stderr  -> Rstderr
  code <- gsub('\\bprintf\\b', 'Rprintf',  code) # printf  -> Rprintf
  code <- gsub('\\bfprintf\\b','Rfprintf', code) # fprintf -> Rfprintf
  code <- gsub('\\bfputs\\b',  'Rfputs',   code) # fputs   -> Rfputs
  code <- gsub('\\babort\\b',  'Rabort',   code) # abort   -> Rabort
  code <- gsub('\\bexit\\b',   'Rexit',    code) # exit    -> Rexit
  writeChar(code, c_file, eos = NULL)
}


# Remove block comments from header files, but retain copyright/license.
h_files <- list.files(c("src", "hl"), "\\.h$", full.names = TRUE, recursive = TRUE)
cat('Minifying', length(h_files), 'header files...\n')
for (h_file in h_files) {
  code <- readChar(h_file, file.size(h_file))
  code <- gsub('(?s)(?<!^) */\\*.*?\\*/', '', code, perl = TRUE)
  code <- gsub('\\n+', '\n', code)
  writeChar(code, h_file, eos = NULL)
}


# Create the HDF5 tarball that will ship with hdf5lib.
setwd('..')
utils::tar('hdf5.tar.gz', 'hdf5', 'gzip', compression_level = 9)


# Remove temporary files
unlink('hdf5', recursive = TRUE)
invisible(file.remove(tarfile))

cat('Done.\n')
