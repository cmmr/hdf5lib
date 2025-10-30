# Use this file to download and minify the latest version of zlib.
# The output of this script should be saved to src/zlib.tar.gz and
# bundled with the hdf5lib source package.


VER <- '1.3.1'
TAR <- paste0('zlib-', VER, '.tar.gz')
PKG <- getwd()
TMP <- tempdir()

cat('Using tempdir:', TMP, '\n')

# Download and extract the zlib codebase
url <- paste0('https://github.com/madler/zlib/releases/download/v', VER, '/', TAR)
cat('Downloading', url, '...\n')
download.file(url = url, destfile = file.path(TMP, TAR))

cat('Extracting zlib codebase...\n')
utils::untar(tarfile = file.path(TMP, TAR), exdir = TMP)
setwd(file.path(TMP, paste0('zlib-', VER)))


# Remove extraneous files and folders
cat('Removing extraneous files and folders...\n')
rm_files <- c(
  'ChangeLog', 'CMakeLists.txt', 'FAQ', 'INDEX', 'make_vms.com', 
  'README', 'treebuild.xml', 'zconf.h.cmakein', 'zlib.3.pdf' )
rm_dirs <- setdiff(list.dirs(recursive = FALSE), './win32')
invisible(file.remove(rm_files))
unlink(rm_dirs, recursive = TRUE)


# Create the zlib tarball that will ship with hdf5lib.
cat('Creating zlib.tar.gz...\n')
setwd(TMP)
invisible(file.rename(paste0('zlib-', VER), 'zlib'))
destfile <- file.path(PKG, 'src', 'zlib.tar.gz')
utils::tar(destfile, 'zlib', 'gzip', compression_level = 9)
cat('Patched zlib is in', destfile, '\n')


# Remove temporary files
cat('Cleaning up...\n')
unlink('zlib', recursive = TRUE)
invisible(file.remove(TAR))

# Return to original working directory
setwd(PKG)

cat('Done.\n')
