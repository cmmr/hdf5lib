#include <blosc2.h>
#include <blosc2/codecs-registry.h>

/* --- External declarations from Blosc2's internal blosc2-zfp.c --- */
extern int zfp_acc_compress(const uint8_t* input, int32_t input_len, uint8_t* output, 
                            int32_t output_len, uint8_t meta, blosc2_cparams* cparams, const void* chunk);
extern int zfp_acc_decompress(const uint8_t* input, int32_t input_len, uint8_t* output, 
                              int32_t output_len, uint8_t meta, blosc2_dparams* dparams, const void* chunk);

extern int zfp_prec_compress(const uint8_t* input, int32_t input_len, uint8_t* output, 
                             int32_t output_len, uint8_t meta, blosc2_cparams* cparams, const void* chunk);
extern int zfp_prec_decompress(const uint8_t* input, int32_t input_len, uint8_t* output, 
                               int32_t output_len, uint8_t meta, blosc2_dparams* dparams, const void* chunk);

extern int zfp_rate_compress(const uint8_t* input, int32_t input_len, uint8_t* output, 
                             int32_t output_len, uint8_t meta, blosc2_cparams* cparams, const void* chunk);
extern int zfp_rate_decompress(const uint8_t* input, int32_t input_len, uint8_t* output, 
                               int32_t output_len, uint8_t meta, blosc2_dparams* dparams, const void* chunk);

/* --- Codec Registration Structs --- */
blosc2_codec zfp_acc_codec = {
    .compcode = BLOSC_CODEC_ZFP_FIXED_ACCURACY, /* 33 */
    .compname = "zfp_acc",
    .complib = 1,
    .version = 1,
    .encoder = zfp_acc_compress,
    .decoder = zfp_acc_decompress
};

blosc2_codec zfp_prec_codec = {
    .compcode = BLOSC_CODEC_ZFP_FIXED_PRECISION, /* 34 */
    .compname = "zfp_prec",
    .complib = 1,
    .version = 1,
    .encoder = zfp_prec_compress,
    .decoder = zfp_prec_decompress
};

blosc2_codec zfp_rate_codec = {
    .compcode = BLOSC_CODEC_ZFP_FIXED_RATE, /* 35 */
    .compname = "zfp_rate",
    .complib = 1,
    .version = 1,
    .encoder = zfp_rate_compress,
    .decoder = zfp_rate_decompress
};
