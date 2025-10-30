# Use this file to download and minify the latest version of zlib.
# The output of this script should be saved to src/zlib.tar.gz and
# bundled with the hdf5lib source package.


VER <- '1.3.1'


# Download and extract the zlib codebase
url <- paste0('https://github.com/madler/zlib/releases/download/v', VER, '/zlib-', VER, '.tar.gz')
tarfile <- basename(url)
download.file(url, tarfile)

cat('Decompressing', tarfile, '...\n')
utils::untar(tarfile)
invisible(file.rename(paste0("zlib-", VER), 'zlib'))
setwd('zlib')


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
setwd('..')
utils::tar('zlib.tar.gz', 'zlib', 'gzip', compression_level = 9)


# Remove temporary files
cat('Cleaning up...\n')
unlink('zlib', recursive = TRUE)
invisible(file.remove(TAR))

cat('Done.\n')
