# Use this file to download and minify the latest version of blosc2.
# The output of this script should be saved to src/c-blosc2-VER.tar.gz and
# bundled with the hdf5lib source package.


VER <- '2.23.1'


# Download and extract the blosc2 codebase
baseurl <- "https://github.com/Blosc/c-blosc2/archive/refs/tags/"
url     <- paste0(baseurl, 'v', VER, '.tar.gz')
tarfile <- file.path('src', paste0("c-blosc2-", VER, ".tar.gz"))
exdir   <- file.path('src', paste0("c-blosc2-", VER))
cached  <- file.path('src', paste0("c-blosc2-", VER, "-orig.tar.gz"))

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
  files   = c(
    grep('/blosc/.+\\.h$', utils::untar(tarfile, list=TRUE), value = TRUE),
    paste0(paste0('c-blosc2-', VER, "/"), c(
      'LICENSE.txt', 'include',
      paste0('blosc/', c(
        'blosc2.c', 'blosclz.c', 'blosc2-stdio.c', 'directories.c', 
        'fastcopy.c', 'stune.c', 'timestamp.c', 'schunk.c', 'frame.c', 
        'sframe.c', 'b2nd.c', 'b2nd_utils.c', 'delta.c', 'trunc-prec.c', 
        'shuffle.c', 'shuffle-generic.c', 'bitshuffle-generic.c' )),
      paste0('plugins/codecs/ndlz/', c(
        'ndlz.h', 'ndlz.c', 'ndlz-private.h', 'xxhash.h', 'xxhash.c',
        'ndlz4x4.h', 'ndlz4x4.c', 'ndlz8x8.h', 'ndlz8x8.c' )),
      paste0('plugins/codecs/zfp/', c(
        'blosc2-zfp.h', 'blosc2-zfp.c', 'zfp-private.h' ))
    ))))



# Apply patches
cat('Applying c-blosc2 patches files...\n')
patch_dir   <- paste0("src/patches/c-blosc2-", VER)
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
    if (grepl('#include', code, fixed = TRUE)) {
      code <- sub('#include', '#include <r_compat.h>\n#include', code, fixed = TRUE)
    }
    else {
      code <- paste0('#include <r_compat.h>\n', code)
    }
    
    writeChar(code, c_file, eos = NULL)
  }
}


# Create the blosc2 tarball that will ship with hdf5lib.
cat(paste0("Creating 'c-blosc2-", VER, ".tar.gz'...\n"))
invisible(file.remove(tarfile))
setwd('src')
utils::tar(
  tarfile = paste0("filters/c-blosc2-", VER, ".tar.gz"), 
  files   = paste0("c-blosc2-", VER), 
  compression = "gzip", compression_level = 9 )
setwd('..')


# Remove temporary files
cat('Cleaning up...\n')
unlink(exdir, recursive = TRUE)

cat('Done.\n')
