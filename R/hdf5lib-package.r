#' hdf5lib: A Standalone HDF5 C Library for R
#'
#' @description
#' `hdf5lib` provides a self-contained, static build of the HDF5 C library
#' (version 1.14.6) and zlib (version 1.3.1).
#'
#' @details
#' This package provides no R functions and is intended for R package
#' developers to use in the `LinkingTo` field of their `DESCRIPTION` file.
#' It allows other R packages to easily link against HDF5 without
#' requiring users to install any system-level dependencies.
#'
#' @docType package
#' @name hdf5lib-package
#' @_PACKAGE
NULL