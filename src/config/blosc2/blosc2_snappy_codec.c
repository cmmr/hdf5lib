#include <blosc2.h>
#include <snappy-c.h>

/* Blosc2 expects a specific function signature for encoders */
static int snappy_blosc2_encoder(const uint8_t *input, int32_t input_len, 
                                 uint8_t *output, int32_t output_len, 
                                 uint8_t meta, blosc2_cparams *cparams, 
                                 const void* chunk) {
    size_t comp_len = (size_t)output_len;
    snappy_status status = snappy_compress((const char*)input, (size_t)input_len, 
                                           (char*)output, &comp_len);
    if (status != SNAPPY_OK) return 0; // 0 tells Blosc2 it failed/didn't fit
    return (int)comp_len;
}

/* Blosc2 expects a specific function signature for decoders */
static int snappy_blosc2_decoder(const uint8_t *input, int32_t input_len, 
                                 uint8_t *output, int32_t output_len, 
                                 uint8_t meta, blosc2_dparams *dparams, 
                                 const void* chunk) {
    size_t uncomp_len = (size_t)output_len;
    snappy_status status = snappy_uncompress((const char*)input, (size_t)input_len, 
                                             (char*)output, &uncomp_len);
    if (status != SNAPPY_OK) return 0;
    return (int)uncomp_len;
}

/* Define the codec structure */
blosc2_codec snappy_codec = {
    .compcode = 3, /* 3 is the historical BLOSC_SNAPPY ID */
    .compname = "snappy",
    .complib = 1,
    .version = 1,
    .encoder = snappy_blosc2_encoder,
    .decoder = snappy_blosc2_decoder
};
