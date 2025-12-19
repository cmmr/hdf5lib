# Use this file to download and minify the latest version of zlib.
# The output of this script should be saved to src/zlib-VER.tar.gz and
# bundled with the hdf5lib source package.


VER <- '1.3.1'


# Download and extract the zlib codebase
baseurl <- "https://github.com/madler/zlib/releases/download/"
url     <- paste0(baseurl, 'v', VER, '/zlib-', VER, '.tar.gz')
tarfile <- file.path('src', paste0("zlib-", VER, ".tar.gz"))
exdir   <- file.path('src', paste0("zlib-", VER))
cached  <- file.path('src', paste0("zlib-", VER, "-orig.tar.gz"))

if (file.exists(cached)) {
  file.copy(from = cached, to = tarfile, overwrite = TRUE)
} else {
  cat("Downloading '", tarfile, "'...\n")
  if (file.exists(tarfile)) invisible(file.remove(tarfile))
  download.file(url = url, destfile = tarfile, quiet = TRUE)
}


cat("Decompressing '", tarfile, "'...\n")
utils::untar(tarfile = tarfile, exdir = dirname(tarfile))


# Remove extraneous files and folders
cat('Removing extraneous files and folders...\n')
unlink(
  recursive = TRUE,
  expand    = FALSE,
  x         = file.path(exdir, c(
    'ChangeLog', 'CMakeLists.txt', 'FAQ', 'INDEX', 'make_vms.com', 
    'README', 'treebuild.xml', 'zconf.h.cmakein', 'zlib.3.pdf',
    setdiff(list.dirs(exdir, FALSE, FALSE), 'win32')
  )))


# Apply patches
cat('Applying zlib patches files...\n')
patch_dir   <- file.path('patches', paste0("zlib-", VER))
patch_files <- list.files(patch_dir, ".patch$", full.names = TRUE)
for (patch_file in patch_files) {
  cat("->", basename(patch_file), "\n")
  system2(command = 'patch', args = c('-p0', '-i', patch_file))
}


# Create the zlib tarball that will ship with hdf5lib.
cat(paste0("Creating 'zlib", VER, ".tar.gz'...\n"))
invisible(file.remove(tarfile))
setwd('src')
utils::tar(
  tarfile = paste0("zlib-", VER, ".tar.gz"), 
  files   = paste0("zlib-", VER), 
  compression = "gzip", compression_level = 9 )
setwd('..')


# Remove temporary files
cat('Cleaning up...\n')
unlink(exdir, recursive = TRUE)

cat('Done.\n')
