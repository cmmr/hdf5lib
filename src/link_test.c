#include <hdf5.h>

/* Simple function just to test linking */
/* It won't actually be called by R */
void hdf5lib_link_test(void) {
    // Calling H5open ensures the library is linked
    // We don't need a real file or error checking here.
    H5open();
}
