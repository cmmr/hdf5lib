#ifndef HDF5LIB_FILTERS_H
#define HDF5LIB_FILTERS_H
#include <hdf5.h>

/* Expose the functions to downstream packages */
herr_t hdf5lib_register_all_filters(void);
herr_t hdf5lib_destroy_all_filters(void);

#endif
