#include <blosc2.h>
#include <stdlib.h>
#include <string.h>
#include "csnappy.h"

/* Blosc2 expects a specific function signature for encoders */
static int snappy_blosc2_encoder(const uint8_t *input, int32_t input_len, 
                                 uint8_t *output, int32_t output_len, 
                                 uint8_t meta, blosc2_cparams *cparams, 
                                 const void* chunk) {
    uint32_t max_comp_len = csnappy_max_compressed_length((uint32_t)input_len);
    
    /* csnappy requires a dedicated working memory buffer for compression */
    void *workmem = malloc(CSNAPPY_WORKMEM_BYTES);
    if (!workmem) return 0;

    uint32_t comp_len = 0;

    /* To safely prevent csnappy from overflowing Blosc2's provided output buffer, 
       we compress into a properly sized temporary buffer if output_len is too small. */
    if ((uint32_t)output_len >= max_comp_len) {
        csnappy_compress((const char*)input, (uint32_t)input_len, 
                         (char*)output, &comp_len, 
                         workmem, CSNAPPY_WORKMEM_BYTES_POWER_OF_TWO);
    } else {
        char *temp_out = malloc(max_comp_len);
        if (!temp_out) {
            free(workmem);
            return 0;
        }
        csnappy_compress((const char*)input, (uint32_t)input_len, 
                         temp_out, &comp_len, 
                         workmem, CSNAPPY_WORKMEM_BYTES_POWER_OF_TWO);
        
        /* Only copy the result if the final compressed size actually fits */
        if (comp_len <= (uint32_t)output_len) {
            memcpy(output, temp_out, comp_len);
        } else {
            comp_len = 0; /* 0 tells Blosc2 it failed/didn't fit */
        }
        free(temp_out);
    }

    free(workmem);
    return (int)comp_len;
}

/* Blosc2 expects a specific function signature for decoders */
static int snappy_blosc2_decoder(const uint8_t *input, int32_t input_len, 
                                 uint8_t *output, int32_t output_len, 
                                 uint8_t meta, blosc2_dparams *dparams, 
                                 const void* chunk) {
    uint32_t uncomp_len = 0;
    int status;
    
    /* Extract the exact uncompressed size from the Snappy header */
    status = csnappy_get_uncompressed_length((const char*)input, (uint32_t)input_len, &uncomp_len);
    if (status == CSNAPPY_E_HEADER_BAD) return 0;
    
    /* Ensure the decompressed data will fit in Blosc2's provided output buffer */
    if (uncomp_len > (uint32_t)output_len) return 0;

    status = csnappy_decompress((const char*)input, (uint32_t)input_len, 
                                (char*)output, uncomp_len);
    if (status != CSNAPPY_E_OK) return 0;
    
    return (int)uncomp_len;
}

/* Define the codec structure */
blosc2_codec snappy_codec = {
    .compcode = 2,
    .compname = "snappy",
    .complib = 2,
    .version = 1,
    .encoder = snappy_blosc2_encoder,
    .decoder = snappy_blosc2_decoder
};