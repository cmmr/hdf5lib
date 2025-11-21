# Use this file to download, patch, and minify the latest version of HDF5.
# The output of this script should be saved to src/hdf5-VER.tar.gz and bundled
# with the hdf5lib source package.

VER <- "2.0.0"

# Download and extract the HDF5 codebase
baseurl <- "https://github.com/HDFGroup/hdf5/releases/download/"
url     <- paste0(baseurl, "hdf5_", VER, "/hdf5-", VER, ".tar.gz")
tarfile <- file.path('src', paste0("hdf5-", VER, ".tar.gz"))
exdir   <- file.path('src', paste0("hdf5-", VER))

cat("Downloading '", tarfile, "'...\n")
if (file.exists(tarfile)) invisible(file.remove(tarfile))
download.file(url = url, destfile = tarfile, quiet = TRUE)

cat("Decompressing '", tarfile, "'...\n")
utils::untar(tarfile = tarfile, exdir = dirname(tarfile))


# We've disabled a bunch of features, so no need to bundle that code.
cat('Removing unneeded files...\n')
unlink(
  recursive = TRUE,
  expand    = FALSE,
  x         = file.path(exdir, c(
    "c++", "doxygen", "fortran", "HDF5Examples", "java",
    "release_docs", "test", "testpar", "tools", "utils",
    file.path("hl", c("c++", "fortran", "test", "tools"))
  )))


# Here's where we apply surgical fixes and add new code.
cat('Applying hdf5 patches files...\n')
patch_dir <- file.path('patches', paste0("hdf5-", VER))
for (patch_file in list.files(patch_dir, full.names = TRUE)) {
  cat("->", basename(patch_file), "\n")
  system2(command = 'patch', args = c('-p0', '-i', patch_file))
}


# Remove block comments from header files, but retain copyright/license.
h_files <- list.files(exdir, "\\.h$", full.names = TRUE, recursive = TRUE)
cat('Minifying', length(h_files), 'header files...\n')
for (h_file in h_files) {
  code <- readChar(h_file, file.size(h_file))
  code <- gsub('(?s)(?<!^)/\\*.*?\\*/', '', code, perl = TRUE)
  code <- gsub('\\n\\n+', '\n\n', code)
  writeChar(code, h_file, eos = NULL)
}


# Global search and replace incompatible C functions.
# Also remove block comments.
c_files <- list.files(exdir, "\\.c$", full.names = TRUE, recursive = TRUE)
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
  code <- gsub('(?s)(?<!^)/\\*.*?\\*/', '', code, perl = TRUE)
  code <- gsub('\\n\\n+', '\n\n', code)
  writeChar(code, c_file, eos = NULL)
}


# Create the HDF5 tarball that will ship with hdf5lib.
invisible(file.remove(tarfile))
setwd('src')
utils::tar(
  tarfile = paste0("hdf5-", VER, ".tar.gz"), 
  files   = paste0("hdf5-", VER), 
  compression = "gzip", compression_level = 9 )
setwd('..')


# Remove temporary files
unlink(exdir, recursive = TRUE)

cat('Done.\n')
