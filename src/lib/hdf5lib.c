#include <R_ext/Error.h>
#include <hdf5.h>
#include <blosc.h>
#include <blosc2.h>
#include <blosc2/codecs-registry.h>

/* --- Declare HDF5 Filters --- */
extern const H5Z_class2_t blosc_class;
extern const H5Z_class2_t blosc2_class;
extern const H5Z_class2_t bshuf_class;
extern const H5Z_class2_t bzip2_class;
extern const H5Z_class2_t lz4_class;
extern const H5Z_class2_t lzf_class;
extern const H5Z_class2_t snappy_class;
extern const H5Z_class2_t zfp_class;
extern const H5Z_class2_t zstd_class;

extern const H5Z_class2_t bitgroom_shim_class;
extern const H5Z_class2_t bitshave_shim_class;
extern const H5Z_class2_t bitround_shim_class;

/* --- Declare C-Blosc2 Globals & Codecs --- */
extern blosc2_codec ndlz_codec;
extern blosc2_codec zfp_prec_codec;
extern blosc2_codec zfp_acc_codec;
extern blosc2_codec zfp_rate_codec;

/* --- Internal Blosc2 Registration (Bypasses User ID Boundary) --- */
extern int register_codec_private(blosc2_codec *codec);

/* --- Registration Macros --- */
#define REG_BLOSC2_CODEC(codec_ptr, name_str) do { \
    if (register_codec_private(codec_ptr) < 0) { \
        Rf_warning("[HDF5LIB ERROR]: Failed to register Blosc2 codec: %s", name_str); \
        err = -1; \
    } \
} while(0)

#define REG_HDF5_FILTER(filter_ptr, name_str) do { \
    if (H5Zregister(filter_ptr) < 0) { \
        Rf_warning("[HDF5LIB ERROR]: Failed to register HDF5 filter: %s", name_str); \
        err = -1; \
    } \
} while(0)


/* --- Registration Function --- */
herr_t hdf5lib_register_all_filters(void) {
    herr_t err = 0;

    /* Initialize Blosc engines globally */
    blosc_init();
    blosc2_init();

    /* Use the standard API for modern codecs (IDs >= 32) and verify success */
    REG_BLOSC2_CODEC(&ndlz_codec, "ndlz");
    REG_BLOSC2_CODEC(&zfp_prec_codec, "zfp_prec");
    REG_BLOSC2_CODEC(&zfp_acc_codec, "zfp_acc");
    REG_BLOSC2_CODEC(&zfp_rate_codec, "zfp_rate");

    /* Register the standalone HDF5 plugins and verify success */
    REG_HDF5_FILTER(&blosc_class, "blosc");
    REG_HDF5_FILTER(&blosc2_class, "blosc2");
    REG_HDF5_FILTER(&bshuf_class, "bshuf");
    REG_HDF5_FILTER(&bzip2_class, "bzip2");
    REG_HDF5_FILTER(&lz4_class, "lz4");
    REG_HDF5_FILTER(&lzf_class, "lzf");
    REG_HDF5_FILTER(&snappy_class, "snappy");
    REG_HDF5_FILTER(&zfp_class, "zfp");
    REG_HDF5_FILTER(&zstd_class, "zstd");

    REG_HDF5_FILTER(&bitgroom_shim_class, "bitgroom_read_shim");
    REG_HDF5_FILTER(&bitshave_shim_class, "bitshave_read_shim");
    REG_HDF5_FILTER(&bitround_shim_class, "bitround_read_shim");

    return err;
}


/* --- Cleanup Function --- */
herr_t hdf5lib_destroy_all_filters(void) {

  /* Unregister standalone HDF5 plugins to prevent dangling pointers 
     if the R package's shared object is dynamically unloaded. */
  H5Zunregister(blosc_class.id);
  H5Zunregister(blosc2_class.id);
  H5Zunregister(bshuf_class.id);
  H5Zunregister(bzip2_class.id);
  H5Zunregister(lz4_class.id);
  H5Zunregister(lzf_class.id);
  H5Zunregister(snappy_class.id);
  H5Zunregister(zfp_class.id);
  H5Zunregister(zstd_class.id);

  /* Safely tear down the Blosc thread pools and TLS memory */
  blosc_destroy();
  blosc2_destroy();
  
  return 0;
}
