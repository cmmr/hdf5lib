#include <hdf5.h>
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

/* --- Declare External Encoders/Decoders for Blosc2 Codecs --- */
/* (These functions exist inside ndlz.o and blosc2-zfp.o) */
extern int ndlz_compress(const uint8_t* input, int32_t input_len, uint8_t* output, int32_t output_len, uint8_t meta, blosc2_cparams* cparams, const void* chunk);
extern int ndlz_decompress(const uint8_t* input, int32_t input_len, uint8_t* output, int32_t output_len, uint8_t meta, blosc2_dparams* dparams, const void* chunk);

extern int zfp_prec_compress(const uint8_t* input, int32_t input_len, uint8_t* output, int32_t output_len, uint8_t meta, blosc2_cparams* cparams, const void* chunk);
extern int zfp_prec_decompress(const uint8_t* input, int32_t input_len, uint8_t* output, int32_t output_len, uint8_t meta, blosc2_dparams* dparams, const void* chunk);


/* --- Define Blosc2 Codecs natively --- */

extern blosc2_codec snappy_codec; /* Located in blosc2_snappy_codec.c */

blosc2_codec ndlz_codec = {
  .compcode = BLOSC_CODEC_NDLZ,
  .compname = "ndlz",
  .version  = 1,
  .complib  = BLOSC_CODEC_NDLZ,
  .encoder  = ndlz_compress,
  .decoder  = ndlz_decompress
};

blosc2_codec zfp_prec_codec = {
  .compcode = BLOSC_CODEC_ZFP_FIXED_PRECISION,
  .compname = "zfp_prec",
  .version  = 1,
  .complib  = BLOSC_CODEC_ZFP_FIXED_PRECISION,
  .encoder  = zfp_prec_compress,
  .decoder  = zfp_prec_decompress
};

/* --- Registration Function --- */
herr_t hdf5lib_register_all_filters(void) {
  herr_t err = 0;

  /* 1. Initialize Blosc2 engine globally */
  blosc2_init();
  
  /* 2. Shoehorn custom static codecs into Blosc2 natively */
  blosc2_register_codec(&snappy_codec);
  blosc2_register_codec(&ndlz_codec);
  blosc2_register_codec(&zfp_prec_codec);
  
  /* 3. Register the standalone HDF5 plugins */
  if (H5Zregister(&blosc_class)  < 0) err = -1;
  if (H5Zregister(&blosc2_class) < 0) err = -1;
  if (H5Zregister(&bshuf_class)  < 0) err = -1;
  if (H5Zregister(&bzip2_class)  < 0) err = -1;
  if (H5Zregister(&lz4_class)    < 0) err = -1;
  if (H5Zregister(&lzf_class)    < 0) err = -1;
  if (H5Zregister(&snappy_class) < 0) err = -1;
  if (H5Zregister(&zfp_class)    < 0) err = -1;
  if (H5Zregister(&zstd_class)   < 0) err = -1;
  
  return err;
}


/* --- Cleanup Function --- */
herr_t hdf5lib_destroy_all_filters(void) {
  /* Safely tear down the Blosc thread pool and TLS memory */
  blosc2_destroy();
  return 0;
}
