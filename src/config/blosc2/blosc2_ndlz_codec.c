#include <blosc2.h>
#include <blosc2/codecs-registry.h>

/* --- External declarations from Blosc2's internal ndlz.c --- */
extern int ndlz_compress(const uint8_t* input, int32_t input_len, uint8_t* output, 
                         int32_t output_len, uint8_t meta, blosc2_cparams* cparams, 
                         const void* chunk);
extern int ndlz_decompress(const uint8_t* input, int32_t input_len, uint8_t* output, 
                           int32_t output_len, uint8_t meta, blosc2_dparams* dparams, 
                           const void* chunk);

/* --- Codec Registration Struct --- */
blosc2_codec ndlz_codec = {
    .compcode = BLOSC_CODEC_NDLZ, /* 32 */
    .compname = "ndlz",
    .complib = 1, 
    .version = 1,
    .encoder = ndlz_compress,
    .decoder = ndlz_decompress
};
