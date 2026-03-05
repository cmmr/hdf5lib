# Use this file to download and minify the latest version of zstd.
# The output of this script should be saved to src/zstd-VER.tar.gz and
# bundled with the hdf5lib source package.


VER <- '1.5.7'


# Download and extract the zstd codebase
baseurl <- "https://github.com/facebook/zstd/releases/download/"
url     <- paste0(baseurl, 'v', VER, '/zstd-', VER, '.tar.gz')
tarfile <- file.path('src', paste0("zstd-", VER, ".tar.gz"))
exdir   <- file.path('src', paste0("zstd-", VER))
cached  <- file.path('src', paste0("zstd-", VER, "-orig.tar.gz"))

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
  files   = paste0(paste0("zstd-", VER, "/"), c(
    'LICENSE', 'lib/zstd.h', 'lib/zstd_errors.h',
    'lib/common', 'lib/compress', 'lib/decompress' )))


# # Apply patches
# cat('Applying zstd patches files...\n')
# patch_dir   <- file.path('patches', paste0("zstd-", VER))
# patch_files <- list.files(patch_dir, ".patch$", full.names = TRUE)
# for (patch_file in patch_files) {
#   cat("->", basename(patch_file), "\n")
#   system2(command = 'patch', args = c('-p0', '-i', patch_file))
# }


# Create the zstd tarball that will ship with hdf5lib.
cat(paste0("Creating 'zstd-", VER, ".tar.gz'...\n"))
invisible(file.remove(tarfile))
setwd('src')
utils::tar(
  tarfile = paste0("zstd-", VER, ".tar.gz"), 
  files   = paste0("zstd-", VER), 
  compression = "gzip", compression_level = 9 )
setwd('..')


# Remove temporary files
cat('Cleaning up...\n')
unlink(exdir, recursive = TRUE)

cat('Done.\n')
