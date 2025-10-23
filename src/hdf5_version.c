#include <R.h>
#include <Rinternals.h>
#include <stdio.h>  // snprintf
#include <stdlib.h> // malloc, free

#include <hdf5.h>
#include <hdf5_hl.h>

/*
 * Internal test function callable from R via .Call()
 * - Gets HDF5 library version.
 * - Writes version to dataset "/version_str" in temp file using HL API.
 * - Reads version back from dataset "/version_str" using Low-Level API.
 * - Returns the read-back version string as an R character vector SEXP.
 * - Assumes the temp file is created and deleted by the calling R code.
 */
SEXP C_hdf5_version(SEXP sexp_filename) {

    const char *filename = CHAR(STRING_ELT(sexp_filename, 0));
    char        version_str_write[64];
    char       *version_str_read = NULL;
    unsigned    majnum, minnum, relnum;
    hid_t       file_id  = H5I_INVALID_HID;
    hid_t       dset_id  = H5I_INVALID_HID;
    hid_t       dtype_id = H5I_INVALID_HID;
    herr_t      status      = 0;
    size_t      dtype_size  = 0;
    SEXP        sexp_result = R_NilValue; // Default return

    // --- Get HDF5 Version ---
    if (H5get_libversion(&majnum, &minnum, &relnum) < 0) {
        Rf_error("C_hdf5_version: H5get_libversion failed");
        return R_NilValue;
    }
    snprintf(version_str_write, sizeof(version_str_write), "%u.%u.%u", majnum, minnum, relnum);

    // --- Write using High-Level API ---
    // Create the file
    file_id = H5Fcreate(filename, H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
    if (file_id < 0) {
        Rf_error("C_hdf5_version: H5Fcreate failed for file: %s", filename);
        goto error_cleanup;
    }

    // Write the version string using H5LT
    status = H5LTmake_dataset_string(file_id, "/version_str", version_str_write);
    if (status < 0) {
        Rf_error("C_hdf5_version: H5LTmake_dataset_string failed");
        goto error_cleanup;
    }

    // Close the file
    status = H5Fclose(file_id);
    file_id = H5I_INVALID_HID; // Reset ID after close
    if (status < 0) {
        Rf_warning("C_hdf5_version: H5Fclose (after write) failed");
        // Continue to read attempt anyway
    }

    // --- Read using Low-Level API ---
    // Open the file read-only
    file_id = H5Fopen(filename, H5F_ACC_RDONLY, H5P_DEFAULT);
    if (file_id < 0) {
        Rf_error("C_hdf5_version: H5Fopen failed for file: %s", filename);
        goto error_cleanup;
    }

    // Open the dataset
    dset_id = H5Dopen2(file_id, "/version_str", H5P_DEFAULT);
    if (dset_id < 0) {
        Rf_error("C_hdf5_version: H5Dopen2 failed for dataset '/version_str'");
        goto error_cleanup;
    }

    // Get the datatype
    dtype_id = H5Dget_type(dset_id);
    if (dtype_id < 0) {
        Rf_error("C_hdf5_version: H5Dget_type failed");
        goto error_cleanup;
    }
    // Check if it's actually a string type (optional but good practice)
    if (H5Tget_class(dtype_id) != H5T_STRING) {
         Rf_error("C_hdf5_version: Dataset is not a string type");
         goto error_cleanup;
    }

    // Get the size needed to store the string in memory
    dtype_size = H5Tget_size(dtype_id);
    if (dtype_size == 0) {
         Rf_error("C_hdf5_version: H5Tget_size returned 0");
         goto error_cleanup;
    }

    // Allocate buffer for reading (+1 for null terminator)
    version_str_read = (char *)malloc(dtype_size + 1);
    if (version_str_read == NULL) {
        Rf_error("C_hdf5_version: Failed to allocate memory for reading");
        goto error_cleanup;
    }

    // Read the dataset
    // Need to specify the memory datatype as C string (variable length)
    // Create a C string datatype for reading
    hid_t mem_dtype_id = H5Tcopy(H5T_C_S1);
    H5Tset_size(mem_dtype_id, dtype_size + 1); // Allow space for null term
    H5Tset_strpad(mem_dtype_id, H5T_STR_NULLTERM);

    status = H5Dread(dset_id, mem_dtype_id, H5S_ALL, H5S_ALL, H5P_DEFAULT, version_str_read);

    H5Tclose(mem_dtype_id); // Close the temporary memory datatype

    if (status < 0) {
        Rf_error("C_hdf5_version: H5Dread failed");
        goto error_cleanup;
    }
    // Ensure null termination (H5Dread should do this with H5T_STR_NULLTERM, but belt-and-suspenders)
    version_str_read[dtype_size] = '\0';

    // --- Prepare Return Value ---
    sexp_result = PROTECT(Rf_allocVector(STRSXP, 1));
    SET_STRING_ELT(sexp_result, 0, Rf_mkChar(version_str_read));

error_cleanup:
    // --- Clean up HDF5 objects ---
    if (dtype_id >= 0) H5Tclose(dtype_id);
    // dataspace not used directly here, H5S_ALL handled it
    if (dset_id >= 0) H5Dclose(dset_id);
    if (file_id >= 0) H5Fclose(file_id);
    if (version_str_read != NULL) free(version_str_read);

    UNPROTECT(1);
    return sexp_result;
}
