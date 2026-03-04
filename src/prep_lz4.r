# Use this file to download and minify the latest version of libaec.
# The output of this script should be saved to src/libaec-VER.tar.gz and
# bundled with the hdf5lib source package.


VER <- '1.10.0'


# Download and extract the lz4 codebase
baseurl <- "https://github.com/lz4/lz4/releases/download/"
url     <- paste0(baseurl, 'v', VER, '/lz4-', VER, '.tar.gz')
tarfile <- file.path('src', paste0("lz4-", VER, ".tar.gz"))
exdir   <- file.path('src', paste0("lz4-", VER))
cached  <- file.path('src', paste0("lz4-", VER, "-orig.tar.gz"))

if (file.exists(cached)) {
  file.copy(from = cached, to = tarfile, overwrite = TRUE)
} else {
  cat("Downloading '", tarfile, "'...\n")
  if (file.exists(tarfile)) invisible(file.remove(tarfile))
  download.file(url = url, destfile = tarfile, quiet = TRUE)
  file.copy(from = tarfile, to = cached)
}


cat("Decompressing '", tarfile, "'...\n")
utils::untar(
  tarfile = tarfile,
  exdir   = dirname(tarfile),
  files   = paste0(paste0("lz4-", VER, "/lib/"), c(
    'LICENSE', 'lz4.c', 'lz4.h', 'lz4hc.c', 'lz4hc.h' )))

# Flatten directory structure
for (i in list.files(exdir, recursive = TRUE)) {
  file.rename(file.path(exdir, i), file.path(exdir, basename(i)))
}
unlink(file.path(exdir, 'lib'), recursive = TRUE)



# # Apply patches
# cat('Applying lz4 patches files...\n')
# patch_dir   <- file.path('patches', paste0("lz4-", VER))
# patch_files <- list.files(patch_dir, ".patch$", full.names = TRUE)
# for (patch_file in patch_files) {
#   cat("->", basename(patch_file), "\n")
#   system2(command = 'patch', args = c('-p0', '-i', patch_file))
# }



# Create the lz4 tarball that will ship with hdf5lib.
cat(paste0("Creating 'lz4-", VER, ".tar.gz'...\n"))
invisible(file.remove(tarfile))
setwd('src')
utils::tar(
  tarfile = paste0("lz4-", VER, ".tar.gz"), 
  files   = paste0("lz4-", VER), 
  compression = "gzip", compression_level = 9 )
setwd('..')


# Remove temporary files
cat('Cleaning up...\n')
unlink(exdir, recursive = TRUE)

cat('Done.\n')
