#include <hdf5.h>

/* Declare the classes (these live in the plugin .c files) */
extern const H5Z_class2_t blosc_class;
extern const H5Z_class2_t bzip2_class;
extern const H5Z_class2_t lz4_class;
extern const H5Z_class2_t zstd_class;

/* A standard C function, NOT an R SEXP function */
herr_t hdf5lib_register_all_filters(void) {
  herr_t err = 0;
  
  if (H5Zregister(&blosc_class) < 0) err = -1;
  if (H5Zregister(&bzip2_class) < 0) err = -1;
  if (H5Zregister(&lz4_class) < 0)   err = -1;
  if (H5Zregister(&zstd_class) < 0)  err = -1;
  
  return err;
}
