/**
 * @file noop_plugin.c
 * @brief No-op (Read-Only) HDF5 Filter Shims for Quantization Algorithms
 *
 * BACKGROUND & REASONING:
 * When datasets are written with quantization filters (like BitGroom, BitShave, 
 * or Granular BitRound) using the HDF5 filter pipeline, HDF5 stamps the chunk 
 * headers with the respective filter ID. Because quantization alters the IEEE 754 
 * floating-point bits in place (a form of lossy compression), the data written to 
 * disk remains in a standard floating-point format. 
 *
 * During the read process, there is no mathematical "decompression" or "decoding" 
 * required—the bits are simply read as-is. However, HDF5's strict pipeline rules 
 * mandate that any filter ID found in the chunk header MUST have a corresponding 
 * filter plugin registered in the environment. If the plugin is missing, HDF5 
 * will throw a "filter not found" error and refuse to read the chunk.
 *
 * PURPOSE OF THIS CODE:
 * This file defines "shim" or "no-op" (no operation) filters for these algorithms. 
 * By manually registering these classes in your application (using `H5Zregister()`), 
 * you satisfy HDF5's requirement for the filter to exist. The `noop_filter` function 
 * simply catches the read request, leaves the data buffer completely unmodified, 
 * and returns success. This allows your application to read quantized datasets 
 * out-of-the-box without needing to compile or install heavy, third-party 
 * quantization libraries.
 *
 * USAGE:
 * Call `H5Zregister(&bitgroom_shim_class);` (etc.) in your application code 
 * during initialization, before opening the HDF5 file.
 */

#include <hdf5.h>

/* * Standard Community Codec Repository (CCR) IDs for Quantization.
 * Note: If your writer used custom or experimental IDs for BitShave/BitRound, 
 * update these macros to match the missing filter IDs in your HDF5 error logs.
 */
#define H5Z_FILTER_BITGROOM 32022
#define H5Z_FILTER_BITSHAVE 32023 
#define H5Z_FILTER_BITROUND 32024 

/**
 * @brief The shared no-op filter function.
 * * Intercepts the HDF5 filter pipeline. On read, it passes the buffer unmodified.
 * On write, it gracefully fails since these shims do not contain the actual 
 * quantization logic.
 */
static size_t noop_filter(
    unsigned int flags, size_t cd_nelmts, const unsigned int cd_values[], 
    size_t nbytes, size_t *buf_size, void **buf ) {
  
  /* ----- Decompression (Read) Path ----- */
  if (flags & H5Z_FLAG_REVERSE) {
    /* * The data is already in standard IEEE 754 floating-point format.
     * We do absolutely nothing and pass the buffer through as-is.
     * Returning 'nbytes' tells HDF5 the operation was successful.
     */
    return nbytes;
  }
  
  /* ----- Compression (Write) Path ----- */
  else {
    /* * These shims are designed strictly to enable reading existing data.
     * Implementing the bit-altering math for writing is beyond a no-op shim.
     */
    H5Epush(H5E_DEFAULT, __FILE__, __func__, __LINE__, H5E_ERR_CLS, H5E_PLINE, H5E_UNSUPPORTED, 
            "noop_filter: This plugin is a read-only shim. Writing/Quantization is not supported.");
    return 0; /* 0 indicates failure in HDF5 filters */
  }
}

/* =========================================================================
 * Filter Class Definitions
 * ========================================================================= */

/* BitGroom Shim Class */
const H5Z_class2_t bitgroom_shim_class = { 
  H5Z_CLASS_T_VERS, 
  H5Z_FILTER_BITGROOM, 
  0,                     /* Encoder present flag (0 = gracefully fail on write) */
  1,                     /* Decoder present flag (1 = we can read) */
  "bitgroom_read_shim",  /* Filter name */
  NULL,                  /* can_apply callback */
  NULL,                  /* set_local callback */
  noop_filter            /* The shared no-op function */
};

/* BitShave Shim Class */
const H5Z_class2_t bitshave_shim_class = { 
  H5Z_CLASS_T_VERS, 
  H5Z_FILTER_BITSHAVE, 
  0, 
  1, 
  "bitshave_read_shim", 
  NULL, 
  NULL, 
  noop_filter 
};

/* BitRound Shim Class */
const H5Z_class2_t bitround_shim_class = { 
  H5Z_CLASS_T_VERS, 
  H5Z_FILTER_BITROUND, 
  0, 
  1, 
  "bitround_read_shim", 
  NULL, 
  NULL, 
  noop_filter 
};
