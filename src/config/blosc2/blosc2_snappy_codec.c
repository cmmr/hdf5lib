#include "csnappy.h"
#include <stdlib.h>
#include <stdint.h>

/* Define the status enum expected by c-blosc2's internal snappy calls */
typedef enum {
  SNAPPY_OK = 0,
  SNAPPY_INVALID_INPUT = 1,
  SNAPPY_BUFFER_TOO_SMALL = 2
} snappy_status;

/* * Drop-in replacements for the Google Snappy C API.
 * c-blosc2 will natively link to these at compile-time! 
 */

snappy_status snappy_compress(const char* input,
                              size_t input_length,
                              char* compressed,
                              size_t* compressed_length) {
                                
    void *workmem = malloc(CSNAPPY_WORKMEM_BYTES);
    if (!workmem) return SNAPPY_INVALID_INPUT;
    
    uint32_t comp_len = 0;
    csnappy_compress(input, (uint32_t)input_length, compressed, &comp_len, workmem, CSNAPPY_WORKMEM_BYTES_POWER_OF_TWO);
    *compressed_length = (size_t)comp_len;
    
    free(workmem);
    return SNAPPY_OK;
}

snappy_status snappy_uncompress(const char* compressed,
                                size_t compressed_length,
                                char* uncompressed,
                                size_t* uncompressed_length) {
                                  
    int status = csnappy_decompress(compressed, (uint32_t)compressed_length, uncompressed, (uint32_t)*uncompressed_length);
    if (status != 0) return SNAPPY_INVALID_INPUT;
    return SNAPPY_OK;
}

snappy_status snappy_uncompressed_length(const char* compressed,
                                         size_t compressed_length,
                                         size_t* result) {
                                          
    uint32_t uncomp_len = 0;
    int status = csnappy_get_uncompressed_length(compressed, (uint32_t)compressed_length, &uncomp_len);
    if (status != 0) return SNAPPY_INVALID_INPUT;
    *result = (size_t)uncomp_len;
    return SNAPPY_OK;
}

size_t snappy_max_compressed_length(size_t input_length) {
    return (size_t)csnappy_max_compressed_length((uint32_t)input_length);
}
