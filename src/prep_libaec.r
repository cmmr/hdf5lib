# Use this file to download and minify the latest version of libaec.
# The output of this script should be saved to src/libaec-VER.tar.gz and
# bundled with the hdf5lib source package.


VER <- '1.1.6'


# Download and extract the libaec codebase
baseurl <- "https://github.com/Deutsches-Klimarechenzentrum/libaec/releases/download/"
url     <- paste0(baseurl, 'v', VER, '/libaec-', VER, '.tar.gz')
tarfile <- file.path('src', paste0("libaec-", VER, ".tar.gz"))
exdir   <- file.path('src', paste0("libaec-", VER))
cached  <- file.path('src', paste0("libaec-", VER, "-orig.tar.gz"))

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
  files   = paste0(paste0("libaec-", VER, "/"), c(
    'include/libaec.h',       'include/szlib.h',
    'src/encode.c',           'src/encode.h',
    'src/encode_accessors.c', 'src/encode_accessors.h',
    'src/decode.c',           'src/decode.h',
    'src/vector.c',           'src/vector.h',
    'src/sz_compat.c',        'LICENSE.txt'
  )))
invisible(file.create(file.path(exdir, 'include/config.h')))

# Flatten directory structure
for (i in list.files(exdir, recursive = TRUE)) {
  file.rename(file.path(exdir, i), file.path(exdir, basename(i)))
}
unlink(file.path(exdir, c('include', 'src')), recursive = TRUE)



# Apply patches
cat('Applying libaec patches files...\n')
patch_dir   <- file.path('patches', paste0("libaec-", VER))
patch_files <- list.files(patch_dir, ".patch$", full.names = TRUE)
for (patch_file in patch_files) {
  cat("->", basename(patch_file), "\n")
  system2(command = 'patch', args = c('-p0', '-i', patch_file))
}



# Create the libaec tarball that will ship with hdf5lib.
cat(paste0("Creating 'libaec-", VER, ".tar.gz'...\n"))
invisible(file.remove(tarfile))
setwd('src')
utils::tar(
  tarfile = paste0("libaec-", VER, ".tar.gz"), 
  files   = paste0("libaec-", VER), 
  compression = "gzip", compression_level = 9 )
setwd('..')


# Remove temporary files
cat('Cleaning up...\n')
unlink(exdir, recursive = TRUE)

cat('Done.\n')
