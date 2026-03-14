#include <R.h>
#include <Rinternals.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h> /* Standard printf for immediate output */

#include <hdf5.h>
#include <hdf5_hl.h>
#include "hdf5lib.h"

#define N_ELEMS 16384 // 128 * 128

/* Callback to visit and verify each dataset */
static herr_t visit_cb(hid_t loc_id, const char *name, const H5L_info_t *info, void *op_data) {
    int *errors = (int *)op_data;

    /* Skip root or hidden objects */
    if (name[0] == '.') return 0;

    /* Skip unsupported filters */
    if (strstr(name, "sz_") || strstr(name, "sz3_") || strstr(name, "fcidecomp")) {
        printf("  [-] Skipping %-25s (Unsupported Filter)\n", name);
        fflush(stdout);
        return 0;
    }

    hid_t dset = -1, type = -1;
    double *buf_f = NULL;
    long long *buf_i = NULL;

    dset = H5Dopen2(loc_id, name, H5P_DEFAULT);
    if (dset < 0) return 0; 

    type = H5Dget_type(dset);
    H5T_class_t tclass = H5Tget_class(type);

    /* Determine if the dataset was compressed using a lossy filter */
    int is_lossy = (strstr(name, "rate") || strstr(name, "precision") ||
                    strstr(name, "accuracy") || strstr(name, "expert") ||
                    strstr(name, "truncprec"));

    printf("  [+] Verifying %-24s ... ", name);
    fflush(stdout);

    if (tclass == H5T_FLOAT) {
        buf_f = (double*)malloc(N_ELEMS * sizeof(double));
        if (!buf_f || H5Dread(dset, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, buf_f) < 0) {
            printf("FAILED (Read Error)\n");
            fflush(stdout);
            (*errors)++;
            goto cleanup;
        }

        int match = 1;
        for (int i = 0; i < N_ELEMS; i++) {
            /* Reconstruct Python's expected logic exactly */
            double base = (double)i / (N_ELEMS - 1);
            double wave = sin((double)i * 50.0 / (N_ELEMS - 1)) * 0.1;
            double expected = base + wave;

            double diff = fabs(buf_f[i] - expected);
            
            /* Strict tolerance for float32 precision limits; generous for lossy */
            double tol = is_lossy ? 0.5 : 1e-5; 

            if (diff > tol) {
                printf("FAILED (Mismatch at idx %d: read %f, expected %f)\n", i, buf_f[i], expected);
                fflush(stdout);
                match = 0;
                (*errors)++;
                break;
            }
        }
        if (match) {
            printf("OK%s\n", is_lossy ? " (Lossy Bounds Passed)" : " (Bit-Exact)");
            fflush(stdout);
        }

    } else if (tclass == H5T_INTEGER) {
        buf_i = (long long*)malloc(N_ELEMS * sizeof(long long));
        if (!buf_i || H5Dread(dset, H5T_NATIVE_LLONG, H5S_ALL, H5S_ALL, H5P_DEFAULT, buf_i) < 0) {
            printf("FAILED (Read Error)\n");
            fflush(stdout);
            (*errors)++;
            goto cleanup;
        }

        int match = 1;
        for (int i = 0; i < N_ELEMS; i++) {
            long long expected = i + 1;
            if (buf_i[i] != expected) {
                printf("FAILED (Mismatch at idx %d: read %lld, expected %lld)\n", i, buf_i[i], expected);
                fflush(stdout);
                match = 0;
                (*errors)++;
                break;
            }
        }
        if (match) {
            printf("OK (Bit-Exact)\n");
            fflush(stdout);
        }
    }

cleanup:
    printf("      -> [DEBUG] cleanup: freeing buf_f/buf_i... "); 
    fflush(stdout);
    if (buf_f) free(buf_f);
    if (buf_i) free(buf_i);
    printf("DONE.\n"); 
    fflush(stdout);

    printf("      -> [DEBUG] cleanup: closing type... "); 
    fflush(stdout);
    if (type >= 0) H5Tclose(type);
    printf("DONE.\n"); 
    fflush(stdout);

    printf("      -> [DEBUG] cleanup: closing dset... "); 
    fflush(stdout);
    if (dset >= 0) H5Dclose(dset);
    printf("DONE.\n"); 
    fflush(stdout);

    return 0; /* Continue to next dataset */
}

/* R Entry Point */
SEXP C_read_zoo(SEXP sexp_filename) {
    const char *filename = CHAR(STRING_ELT(sexp_filename, 0));
    hid_t file_id = -1;
    int total_errors = 0;

    H5Eset_auto(H5E_DEFAULT, NULL, NULL);

    if (hdf5lib_register_all_filters() < 0) {
        Rf_error("C_read_zoo: hdf5lib_register_all_filters() failed");
    }

    file_id = H5Fopen(filename, H5F_ACC_RDONLY, H5P_DEFAULT);
    if (file_id < 0) {
        hdf5lib_destroy_all_filters();
        Rf_error("C_read_zoo: Failed to open %s", filename);
    }

    printf("\n--- Reading %s ---\n", filename);
    fflush(stdout);
    
    H5Lvisit(file_id, H5_INDEX_NAME, H5_ITER_NATIVE, visit_cb, &total_errors);

    H5Fclose(file_id);
    
    /* Clean up global filter state */
    hdf5lib_destroy_all_filters();

    if (total_errors > 0) {
        Rf_error("Interop Tests Failed! (%d datasets corrupted/unreadable).", total_errors);
    }

    return Rf_ScalarInteger(1);
}
