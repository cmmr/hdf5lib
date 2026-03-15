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

extern int zfp_acc_compress(const uint8_t* input, int32_t input_len, uint8_t* output, int32_t output_len, uint8_t meta, blosc2_cparams* cparams, const void* chunk);
extern int zfp_acc_decompress(const uint8_t* input, int32_t input_len, uint8_t* output, int32_t output_len, uint8_t meta, blosc2_dparams* dparams, const void* chunk);

extern int zfp_rate_compress(const uint8_t* input, int32_t input_len, uint8_t* output, int32_t output_len, uint8_t meta, blosc2_cparams* cparams, const void* chunk);
extern int zfp_rate_decompress(const uint8_t* input, int32_t input_len, uint8_t* output, int32_t output_len, uint8_t meta, blosc2_dparams* dparams, const void* chunk);

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

blosc2_codec zfp_acc_codec = {
  .compcode = BLOSC_CODEC_ZFP_FIXED_ACCURACY,
  .compname = "zfp_acc",
  .version  = 1,
  .complib  = BLOSC_CODEC_ZFP_FIXED_ACCURACY,
  .encoder  = zfp_acc_compress,
  .decoder  = zfp_acc_decompress
};

blosc2_codec zfp_rate_codec = {
  .compcode = BLOSC_CODEC_ZFP_FIXED_RATE,
  .compname = "zfp_rate",
  .version  = 1,
  .complib  = BLOSC_CODEC_ZFP_FIXED_RATE,
  .encoder  = zfp_rate_compress,
  .decoder  = zfp_rate_decompress
};

/* --- Internal Blosc2 Registration (Bypasses User ID Boundary) --- */
extern int register_codec_private(blosc2_codec *codec);

/* --- Registration Function --- */
herr_t hdf5lib_register_all_filters(void) {
  herr_t err = 0;

  /* 1. Initialize Blosc2 engine globally */
  blosc2_init();
  
  /* 2. Shoehorn custom static codecs into Blosc2 natively using internal API */
  register_codec_private(&snappy_codec);
  register_codec_private(&ndlz_codec);
  register_codec_private(&zfp_prec_codec);
  register_codec_private(&zfp_acc_codec);
  register_codec_private(&zfp_rate_codec);
  
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

  /* Safely tear down the Blosc thread pool and TLS memory */
  blosc2_destroy();
  
  return 0;
}
