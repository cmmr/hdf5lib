# Use this file to download, patch, and minify the latest version of HDF5.
# The output of this script should be saved to src/hdf5-VER.tar.gz and bundled
# with the hdf5lib source package.

VER <- "2.1.1"

# Download and extract the HDF5 codebase
baseurl <- "https://github.com/HDFGroup/hdf5/releases/download/"
url     <- paste0(baseurl, VER, "/hdf5-", VER, ".tar.gz")
tarfile <- file.path('src', paste0("hdf5-", VER, ".tar.gz"))
exdir   <- file.path('src', paste0("hdf5-", VER))
cached  <- file.path('src', paste0("hdf5-", VER, "-orig.tar.gz"))

if (file.exists(cached)) {
  file.copy(from = cached, to = tarfile, overwrite = TRUE)
} else {
  cat("Downloading ", shQuote(tarfile), "...\n")
  if (file.exists(tarfile)) invisible(file.remove(tarfile))
  download.file(url = url, destfile = tarfile, quiet = TRUE)
}

# --- 1. Decompress Only the Core and HL Source Files ---
cat("Decompressing ", shQuote(tarfile), "...\n")
utils::untar(
  tarfile = tarfile, 
  exdir   = dirname(tarfile),
  file    = paste0('./hdf5-', VER, c('/LICENSE', '/src', '/hl/src')) )


# --- 2. Remove Unsupported Driver Files ---
# We explicitly remove sources for features we know we cannot support in CRAN
# (MPI, HDFS, S3, etc.) to prevent accidental compilation.
# NOTE: We must ONLY remove the .c files. The .h files are often included
# by H5FDprivate.h (even if guarded by #ifdefs), so they must exist.
cat('Removing unsupported driver source files...\n')
invisible(file.remove(c(
  list.files(paste0('src/hdf5-', VER, '/src/H5FDsubfiling/'), "*.c", full.names = TRUE),
  paste0('src/hdf5-', VER, c(
    "/src/H5pubconf.h.in", "/src/libhdf5.settings.in",
    "/src/CMakeLists.txt", "/hl/src/CMakeLists.txt",
    "/src/H5ACmpio.c",     "/src/H5build_settings.cmake.c.in",
    "/src/H5FDdirect.c",   "/src/H5build_settings.off.c.in",
    "/src/H5FDhdfs.c",     "/src/H5FDsubfiling/CMakeLists.txt",
    "/src/H5FDmirror.c",   "/src/H5FDmpi.c",
    "/src/H5Cmpio.c",      "/src/H5Dmpio.c",
    "/src/H5FDmpio.c",     "/src/H5Fmpi.c",
    "/src/H5Smpio.c",      "/src/H5mpi.c",
    "/src/H5Obogus.c",     "/src/H5TSc11.c",
    "/src/H5FDros3.c",     "/src/H5FDros3_s3comms.c" )))))


# --- 3. Apply Patches ---
cat('Applying hdf5 patches files...\n')
patch_dir   <- paste0("src/patches/hdf5-", VER)
patch_files <- list.files(patch_dir, ".patch$", full.names = TRUE)
for (patch_file in patch_files) {
  cat("->", basename(patch_file), "\n")
  system2(command = 'patch', args = c('-p0', '-i', patch_file))
}
# Two modifications to the same file generates a *.orig file
invisible(file.remove(list.files(
  path       = paste0('src/hdf5-', VER, '/src'),
  pattern    = '*.orig',
  full.names = TRUE,
  recursive  = TRUE )))


# --- 4. Minify Headers ---
# Remove block comments from header files, but retain copyright/license.
h_files <- list.files(exdir, "\\.h$", full.names = TRUE, recursive = TRUE)
cat('Minifying', length(h_files), 'header files...\n')
for (h_file in h_files) {
  code <- readChar(h_file, file.size(h_file))
  code <- gsub('(?s)(?<!^)/\\*.*?\\*/', '', code, perl = TRUE)
  code <- gsub('\\n\\n+', '\n\n', code)
  writeChar(code, h_file, eos = NULL)
}


# --- 5. Convert C Files to R API ---
# Global search and replace incompatible C functions.
# Also remove block comments.
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

  # Insert <r_compat.h> below copyright header.
  if (!identical(orig, code)) {
    code <- sub('*/', '*/\n\n#include <r_compat.h>\n', code, fixed = TRUE)
  }

  code <- gsub('(?s)(?<!^)/\\*.*?\\*/', '', code, perl = TRUE)
  code <- gsub('\\n\\n+', '\n\n', code)
  writeChar(code, c_file, eos = NULL)
}


# --- 6. Repackage ---
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
