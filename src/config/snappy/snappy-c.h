#ifndef SNAPPY_C_COMPAT_H
#define SNAPPY_C_COMPAT_H

#include "csnappy.h"
#include <stdlib.h>
#include <stdint.h>

/* Map the basic types and macros */
typedef int snappy_status;
#define SNAPPY_OK CSNAPPY_E_OK

/* This maps cleanly with a simple define */
#define snappy_max_compressed_length csnappy_max_compressed_length

/* Wrapper for compression */
static inline snappy_status snappy_compress(const char* input,
                                            size_t input_length,
                                            char* compressed,
                                            size_t* compressed_length) {
    /* csnappy requires a working buffer. 
       CSNAPPY_WORKMEM_BYTES is 64KB (1 << 16). */
    void* working_memory = malloc(CSNAPPY_WORKMEM_BYTES);
    if (!working_memory) return -1; /* Allocation failed */

    uint32_t out_len = 0;
    csnappy_compress(input, (uint32_t)input_length, compressed, &out_len,
                     working_memory, CSNAPPY_WORKMEM_BYTES_POWER_OF_TWO);

    *compressed_length = (size_t)out_len;
    free(working_memory);
    return SNAPPY_OK;
}

/* Wrapper for decompression */
static inline snappy_status snappy_uncompress(const char* compressed,
                                              size_t compressed_length,
                                              char* uncompressed,
                                              size_t* uncompressed_length) {
    uint32_t uncomp_len = 0;
    
    /* First, parse the snappy header to get the exact uncompressed length */
    int status = csnappy_get_uncompressed_length(compressed, (uint32_t)compressed_length, &uncomp_len);
    if (status < 0) return status;

    /* Now decompress safely */
    status = csnappy_decompress(compressed, (uint32_t)compressed_length, uncompressed, uncomp_len);
    if (status == CSNAPPY_E_OK) {
        *uncompressed_length = (size_t)uncomp_len;
    }
    return status;
}

#endif /* SNAPPY_C_COMPAT_H */
