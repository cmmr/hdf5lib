# Use this file to download and minify the latest version of bitshuffle.
# The output of this script should be saved to src/bitshuffle-VER.tar.gz and
# bundled with the hdf5lib source package.


VER <- '0.5.2'


# Download and extract the bitshuffle codebase
baseurl <- "https://github.com/kiyo-masui/bitshuffle/archive/refs/tags/"
url     <- paste0(baseurl, VER, '.tar.gz')
tarfile <- file.path('src', paste0("bitshuffle-", VER, ".tar.gz"))
exdir   <- file.path('src', paste0("bitshuffle-", VER))
cached  <- file.path('src', paste0("bitshuffle-", VER, "-orig.tar.gz"))

if (file.exists(cached)) {
  file.copy(from = cached, to = tarfile, overwrite = TRUE)
} else {
  cat("Downloading '", tarfile, "'...\n")
  if (file.exists(tarfile)) invisible(file.remove(tarfile))
  download.file(url = url, destfile = tarfile, quiet = TRUE)
}


cat("Decompressing '", tarfile, "'...\n")
utils::untar(
  tarfile = tarfile,
  exdir   = dirname(tarfile),
  files   = paste0(paste0("bitshuffle-", VER, "/"), c(
  'LICENSE',
  paste0('src/', c(
    'bitshuffle.c', 'bitshuffle.h', 'bitshuffle_core.c', 'bitshuffle_core.h',
    'bitshuffle_internals.h', 'iochain.c', 'iochain.h' )))))



# Apply patches
cat('Applying bitshuffle patches files...\n')
patch_dir   <- paste0("src/patches/bitshuffle-", VER)
patch_files <- list.files(patch_dir, ".patch$", full.names = TRUE)
for (patch_file in patch_files) {
  cat("->", basename(patch_file), "\n")
  system2(command = 'patch', args = c('-p0', '-i', patch_file))
}



# Convert C Files to R API
# Global search and replace incompatible C functions.
c_files <- list.files(exdir, "\\.c$", full.names = TRUE, recursive = TRUE)
cat('Converting', length(c_files), '.c files to R\'s C API...\n')
for (c_file in c_files) {
  
  code <- orig <- readChar(c_file, file.size(c_file))
  code <- gsub('\\bstdout\\b', 'Rstdout',  code) # stdout  -> Rstdout
  code <- gsub('\\bstderr\\b', 'Rstderr',  code) # stderr  -> Rstderr
  code <- gsub('\\bprintf\\b', 'Rprintf',  code) # printf  -> Rprintf
  code <- gsub('\\bfprintf\\b','Rfprintf', code) # fprintf -> Rfprintf
  code <- gsub('\\bfputs\\b',  'Rfputs',   code) # fputs   -> Rfputs
  code <- gsub('\\babort\\b',  'Rabort',   code) # abort   -> Rabort
  code <- gsub('\\bexit\\b',   'Rexit',    code) # exit    -> Rexit

  if (!identical(orig, code)) {
    code <- paste0('#include <r_compat.h>\n', code)
    writeChar(code, c_file, eos = NULL)
  }
}



# Create the bitshuffle tarball that will ship with hdf5lib.
cat(paste0("Creating 'bitshuffle-", VER, ".tar.gz'...\n"))
invisible(file.remove(tarfile))
setwd('src')
utils::tar(
  tarfile = paste0("filters/bitshuffle-", VER, ".tar.gz"), 
  files   = paste0("bitshuffle-", VER), 
  compression = "gzip", compression_level = 9 )
setwd('..')


# Remove temporary files
cat('Cleaning up...\n')
unlink(exdir, recursive = TRUE)

cat('Done.\n')
