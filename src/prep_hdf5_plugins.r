# Use this file to download and minify the latest version of hdf5_plugins.
# The output of this script should be saved to src/hdf5_plugins-VER/ and
# bundled with the hdf5lib source package.


VER <- '2.1.0'


# Download and extract the hdf5_plugins codebase
baseurl <- "https://github.com/HDFGroup/hdf5_plugins/releases/download/"
url     <- paste0(baseurl, VER, '/hdf5_plugins-', VER, '.tar.gz')
tarfile <- file.path('src', paste0("hdf5_plugins-", VER, ".tar.gz"))
exdir   <- file.path('src', paste0("hdf5_plugins-", VER))
cached  <- file.path('src', paste0("hdf5_plugins-", VER, "-orig.tar.gz"))

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
  files   = paste0(paste0("hdf5_plugins-", VER, "/"), c(
    'LZ4/Additional_Legal/LICENSE',
    'LZ4/src/H5Zlz4.c',
    'ZSTD/Additional_Legal/LICENSE',
    'ZSTD/src/H5Zzstd.c'
  )))
unlink(tarfile)


# # Apply patches
# cat('Applying hdf5_plugins patches files...\n')
# patch_dir   <- file.path('patches', paste0("hdf5_plugins-", VER))
# patch_files <- list.files(patch_dir, ".patch$", full.names = TRUE)
# for (patch_file in patch_files) {
#   cat("->", basename(patch_file), "\n")
#   system2(command = 'patch', args = c('-p0', '-i', patch_file))
# }


# Put each plugin into a separate .tar.gz file
plugin_dirs <- list.dirs(exdir, full.names = TRUE, recursive = FALSE)
for (plugin in plugin_dirs) {
  cat("  -> ", basename(plugin), "\n")
  
  for (i in list.files(plugin, recursive = TRUE)) {
    file.rename(file.path(plugin, i), file.path(plugin, basename(i)))
  }
  rm_dirs <- list.dirs(plugin, full.names = FALSE, recursive = FALSE)
  unlink(file.path(plugin, rm_dirs), recursive = TRUE)
  
  old_dir <- setwd(dirname(plugin))
  utils::tar(
    tarfile = paste0(basename(plugin), ".tar.gz"), 
    files   = basename(plugin), 
    compression = "gzip", compression_level = 9 )
  setwd(old_dir)
}
unlink(plugin_dirs, recursive = TRUE)

cat('Done.\n')
