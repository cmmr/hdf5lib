// Explicitly undefine disabled HDF5 features before including the header
// to prevent R CMD INSTALL compilation errors.
#undef H5_HAVE_SUBFILING_VFD
#undef H5_HAVE_DIRECT_VFD

#include <hdf5.h>

/* Simple function just to test linking */
/* It won't actually be called by R */
void hdf5lib_link_test(void) {
    // Calling H5open ensures the library is linked
    // We don't need a real file or error checking here.
    H5open();
}
