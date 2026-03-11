/**
 * @file lz4_plugin.c
 * @brief Standalone HDF5 Filter Plugin for LZ4 and LZ4HC
 */

#include <hdf5.h>
#include <lz4.h>
#include <lz4hc.h>
#include <stdint.h>
#include <string.h>

#define H5Z_FILTER_LZ4 32004
#define DEFAULT_BLOCK_SIZE (1 << 30) /* 1 GiB blocks for large datasets */

/* LZ4's internal limit for a single compression block is roughly 1.9 GB */
#define LZ4_MAX_SAFE_BLOCK_SIZE 1900000000 

#define PUSH_ERR(...) do { \
    H5Epush(H5E_DEFAULT, __FILE__, __func__, __LINE__, H5E_ERR_CLS, H5E_PLINE, H5E_CANTFILTER, __VA_ARGS__); \
    return 0; \
} while(0)

/* CRAN-safe Big-Endian encoders */
static void write_be64(uint8_t *p, uint64_t val) {
  p[0] = (uint8_t)(val >> 56); p[1] = (uint8_t)(val >> 48);
  p[2] = (uint8_t)(val >> 40); p[3] = (uint8_t)(val >> 32);
  p[4] = (uint8_t)(val >> 24); p[5] = (uint8_t)(val >> 16);
  p[6] = (uint8_t)(val >> 8);  p[7] = (uint8_t)(val & 0xFF);
}

static uint64_t read_be64(const uint8_t *p) {
  return ((uint64_t)p[0] << 56) | ((uint64_t)p[1] << 48) | ((uint64_t)p[2] << 40) | ((uint64_t)p[3] << 32) |
    ((uint64_t)p[4] << 24) | ((uint64_t)p[5] << 16) | ((uint64_t)p[6] << 8)  | ((uint64_t)p[7]);
}

static void write_be32(uint8_t *p, uint32_t val) {
  p[0] = (uint8_t)(val >> 24); p[1] = (uint8_t)(val >> 16);
  p[2] = (uint8_t)(val >> 8);  p[3] = (uint8_t)(val & 0xFF);
}

static uint32_t read_be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | ((uint32_t)p[3]);
}

static size_t lz4_filter(
    unsigned int flags, size_t cd_nelmts, const unsigned int cd_values[], 
    size_t nbytes, size_t *buf_size, void **buf ) {
  
  /* Handle completely empty dataset/chunk edge case */
  if (nbytes == 0) {
    return 0;
  }

  /* Decompression Path */
  if (flags & H5Z_FLAG_REVERSE) {
    if (nbytes < 12) {
      PUSH_ERR("lz4_filter: Compressed chunk too small to contain header");
    }

    const uint8_t *rpos = (const uint8_t *)*buf;
    const uint8_t *r_end = rpos + nbytes; /* Pointer bound for safety */
    
    uint64_t origSize = read_be64(rpos); rpos += 8;
    uint32_t blockSize = read_be32(rpos); rpos += 4;
    
    if (blockSize == 0) PUSH_ERR("lz4_filter: Invalid LZ4 block size 0");
    if (blockSize > origSize) blockSize = (uint32_t)origSize; /* Safety clamp */
    
    void *outBuf = H5allocate_memory((size_t)origSize, 0);
    if (!outBuf) PUSH_ERR("lz4_filter: Memory allocation failed");
    
    uint8_t *wpos = (uint8_t *)outBuf;
    size_t decompSize = 0;
    
    while (decompSize < origSize) {
      if (rpos + 4 > r_end) {
        H5free_memory(outBuf);
        PUSH_ERR("lz4_filter: Corrupt data, unexpected end of buffer reading block size");
      }
      
      uint32_t compBlockSize = read_be32(rpos); rpos += 4;
      
      if (rpos + compBlockSize > r_end) {
        H5free_memory(outBuf);
        PUSH_ERR("lz4_filter: Corrupt data, block size exceeds remaining buffer");
      }
      
      size_t currentBlock = (origSize - decompSize < blockSize) ? (origSize - decompSize) : blockSize;
      
      if (compBlockSize == currentBlock) {
        /* Data was incompressible, stored uncompressed */
        memcpy(wpos, rpos, currentBlock);
      } else {
        /* Decompress block */
        if (LZ4_decompress_safe((const char*)rpos, (char*)wpos, (int)compBlockSize, (int)currentBlock) < 0) {
          H5free_memory(outBuf);
          PUSH_ERR("lz4_filter: LZ4 block decompression failed");
        }
      }
      
      rpos += compBlockSize;
      wpos += currentBlock;
      decompSize += currentBlock;
    }
    
    H5free_memory(*buf); 
    *buf = outBuf;
    *buf_size = (size_t)origSize;
    return (size_t)origSize;
  }
  
  /* Compression Path */
  else {
    size_t blockSize = (cd_nelmts > 0 && cd_values[0] > 0) ? cd_values[0] : DEFAULT_BLOCK_SIZE;
    
    /* Clamp blockSize to bounds LZ4 can safely process */
    if (blockSize > LZ4_MAX_SAFE_BLOCK_SIZE) blockSize = LZ4_MAX_SAFE_BLOCK_SIZE;
    if (blockSize > nbytes) blockSize = nbytes;
    
    /* Check for an HC level in the second slot */
    int hc_level = (cd_nelmts > 1) ? (int)cd_values[1] : 0;
    
    size_t nBlocks = (nbytes - 1) / blockSize + 1;
    size_t maxOut = nBlocks * LZ4_compressBound((int)blockSize) + 12 + (nBlocks * 4);
    
    void *outBuf = H5allocate_memory(maxOut, 0);
    if (!outBuf) PUSH_ERR("lz4_filter: Memory allocation failed");
    
    uint8_t *wpos = (uint8_t *)outBuf;
    
    /* Write Header */
    write_be64(wpos, (uint64_t)nbytes); wpos += 8;
    write_be32(wpos, (uint32_t)blockSize); wpos += 4;
    
    const uint8_t *rpos = (const uint8_t *)*buf;
    size_t bytesLeft = nbytes;
    size_t totalOut = 12;
    
    for (size_t i = 0; i < nBlocks; i++) {
      size_t currentBlock = (bytesLeft < blockSize) ? bytesLeft : blockSize;
      int compSize;
      
      /* Route to HC if requested, otherwise default */
      if (hc_level > 0) {
        compSize = LZ4_compress_HC((const char*)rpos, (char*)wpos + 4, 
                                   (int)currentBlock, LZ4_compressBound((int)currentBlock), hc_level);
      } else {
        compSize = LZ4_compress_default((const char*)rpos, (char*)wpos + 4, 
                                        (int)currentBlock, LZ4_compressBound((int)currentBlock));
      }
      
      /* If compression failed or didn't save space, fallback to memcpy */
      if (compSize >= (int)currentBlock || compSize <= 0) {
        compSize = (int)currentBlock;
        memcpy(wpos + 4, rpos, currentBlock);
      }
      
      write_be32(wpos, (uint32_t)compSize);
      wpos += (compSize + 4);
      rpos += currentBlock;
      bytesLeft -= currentBlock;
      totalOut += (compSize + 4);
    }
    
    H5free_memory(*buf); 
    *buf = outBuf; 
    *buf_size = totalOut;
    return totalOut;
  }
}

const H5Z_class2_t lz4_class = { 
  H5Z_CLASS_T_VERS, 
  H5Z_FILTER_LZ4, 
  1, 
  1, 
  "lz4", 
  NULL, 
  NULL, 
  lz4_filter 
};
