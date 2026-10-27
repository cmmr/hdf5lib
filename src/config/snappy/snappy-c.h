#ifndef SNAPPY_COMPAT_H
#define SNAPPY_COMPAT_H

#include "csnappy.h"
#include <stdint.h>
#include <stdlib.h>

/* Map the basic types and macros */
typedef int snappy_status;
#define SNAPPY_OK CSNAPPY_E_OK

/* This maps cleanly with a simple define */
#define snappy_max_compressed_length csnappy_max_compressed_length

/* FIX: csnappy.h explicitly states: 9 <= workmem_bytes_power_of_two <= 15.
 * The macro in csnappy.h is dangerously set to 16, causing a massive heap
 * buffer overflow because the internal hash table expects 16-bit entries.
 * We clamp the power to 15 and oversize the allocation to 256KB to be safe.
 */
#define SAFE_WORKMEM_POWER 15
#define SAFE_WORKMEM_BYTES (1 << 18) /* 256 KB */

/* Wrapper for compression */
static inline snappy_status snappy_compress(const char* input,
                                            size_t input_length,
                                            char* compressed,
                                            size_t* compressed_length) {
    /* Plain malloc(), not H5allocate_memory(): Blosc calls this from its worker
       threads, and H5allocate_memory() takes HDF5's global lock -- which the
       thread running the filter holds while it waits for those workers. */
    void* working_memory = malloc(SAFE_WORKMEM_BYTES);
    if (!working_memory) return -1; /* Allocation failed */

    uint32_t out_len = 0;
    csnappy_compress(input, (uint32_t)input_length, compressed, &out_len,
                     working_memory, SAFE_WORKMEM_POWER);

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

#endif /* SNAPPY_COMPAT_H */
