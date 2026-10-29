/**
 * @file snappy_plugin.c
 * @brief Standalone HDF5 Filter Plugin for Google Snappy
 *
 * Uses the pure C csnappy port.
 * Registered HDF5 Filter ID: 32003
 *
 * The registry assigns 32003 to snappy-cuda (https://github.com/lucasvr/snappy-cuda),
 * so chunks are written the way its filter writes them:
 *
 *   varint32  uncompressed length of the chunk
 *   varint32  block size (32 KiB, snappy-cuda's default)
 *   for each block: a 4-byte little-endian compressed length, then that many
 *                   bytes of Snappy elements (no length preamble)
 *
 * Every block but the last holds a full block of input; snappy-cuda's GPU
 * decoder relies on that.  There are no cd_values.  hdf5lib 2.2.0.0 and earlier
 * wrote one plain Snappy stream (varint32 length, then elements) per chunk
 * instead, so a chunk that is not laid out as above is read that way.
 */

#include <hdf5.h>
#include "csnappy.h"
#include <stdlib.h>
#include <stdint.h>

#define H5Z_FILTER_SNAPPY 32003

/* csnappy_compress_fragment() takes at most 32 KiB of input and a hash table
   of at most 2^15 bytes */
#define SNAPPY_BLOCK_SIZE    32768
#define SNAPPY_WORKMEM_POWER 15

#define PUSH_ERR(...) do { \
    H5Epush(H5E_DEFAULT, __FILE__, __func__, __LINE__, H5E_ERR_CLS, H5E_PLINE, H5E_CANTFILTER, __VA_ARGS__); \
    return 0; \
} while(0)

/* Reads a varint32 (7 bits per byte, low bits first) from p[0..n).  Returns
   the number of bytes it took, or 0 if it runs past n or overflows 32 bits */
static size_t read_varint32(const uint8_t *p, size_t n, uint32_t *val) {
  uint32_t v = 0;
  for (size_t i = 0; i < 5 && i < n; i++) {
    if (i == 4 && p[i] > 0x0F) return 0;
    v |= (uint32_t)(p[i] & 0x7F) << (7 * i);
    if (!(p[i] & 0x80)) {
      *val = v;
      return i + 1;
    }
  }
  return 0;
}

static size_t write_varint32(uint8_t *p, uint32_t val) {
  size_t n = 0;
  while (val >= 0x80) {
    p[n++] = (uint8_t)(val | 0x80);
    val >>= 7;
  }
  p[n++] = (uint8_t)val;
  return n;
}

static uint32_t read_le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void write_le32(uint8_t *p, uint32_t val) {
  p[0] = (uint8_t)val;         p[1] = (uint8_t)(val >> 8);
  p[2] = (uint8_t)(val >> 16); p[3] = (uint8_t)(val >> 24);
}

/* Whether the Snappy elements in p[0..n) end exactly at n.  When the last one
   is cut short, csnappy_decompress_noheader() completes it from stale bytes
   rather than failing, so a chunk missing its last byte or two would read
   back without an error. */
static int elements_fit(const uint8_t *p, size_t n) {
  static const uint8_t offset_bytes[4] = {0, 1, 2, 4};  /* after a copy's tag, by type */
  size_t pos = 0;
  while (pos < n) {
    uint8_t tag = p[pos++];
    uint64_t skip;
    if ((tag & 3) != 0) {
      skip = offset_bytes[tag & 3];
    } else {
      /* A literal: its length - 1 is in the tag, or in the 1-4 bytes after it */
      skip = (tag >> 2) + 1;
      if (skip > 60) {
        size_t extra = skip - 60;
        if (n - pos < extra) return 0;
        skip = 0;
        for (size_t i = 0; i < extra; i++) skip |= (uint64_t)p[pos + i] << (8 * i);
        skip += 1 + extra;
      }
    }
    if (skip > n - pos) return 0;
    pos += (size_t)skip;
  }
  return 1;
}

/* Whether in[0..nbytes) is laid out as snappy-cuda writes it: a total length,
   a block size, and exactly ceil(total / block size) length-prefixed blocks
   that end where the chunk does.  Sets the two lengths and where the first
   block starts. */
static int is_blocked(const uint8_t *in, size_t nbytes, uint32_t *total, uint32_t *block, size_t *start) {
  size_t a = read_varint32(in, nbytes, total);
  if (a == 0 || *total == 0) return 0;
  size_t b = read_varint32(in + a, nbytes - a, block);
  if (b == 0 || *block == 0) return 0;

  size_t pos = a + b;
  uint32_t nblocks = *total / *block + (*total % *block != 0);
  for (uint32_t i = 0; i < nblocks; i++) {
    if (nbytes - pos < 4) return 0;
    uint32_t len = read_le32(in + pos);
    pos += 4;
    if (len > nbytes - pos) return 0;
    pos += len;
  }
  if (pos != nbytes) return 0;

  *start = a + b;
  return 1;
}

/* Decodes the blocks is_blocked() found into out[0..total).  Each must fill
   its share of the output exactly. */
static int decode_blocks(const uint8_t *in, size_t pos, uint32_t total, uint32_t block, char *out) {
  for (uint32_t done = 0; done < total; ) {
    uint32_t want = (total - done < block) ? total - done : block;
    uint32_t len = read_le32(in + pos);
    uint32_t produced = want;
    if (!elements_fit(in + pos + 4, len)) return 0;
    int status = csnappy_decompress_noheader((const char *)in + pos + 4, len, out + done, &produced);
    if (status != CSNAPPY_E_OK || produced != want) return 0;
    pos += 4 + (size_t)len;
    done += want;
  }
  return 1;
}

static size_t snappy_filter(
    unsigned int flags, size_t cd_nelmts, const unsigned int cd_values[],
    size_t nbytes, size_t *buf_size, void **buf) {

  /* Handle completely empty dataset/chunk edge case */
  if (nbytes == 0) {
    return 0;
  }

  /* Snappy's internal block format relies on 32-bit integers for length.
     We must prevent chunks larger than ~4GB from crashing the compressor. */
  if (nbytes > 0xFFFFFFFF) {
    PUSH_ERR("snappy_filter: Chunk exceeds 4GB Snappy limit");
  }

  /* ----- Decompression Path ----- */
  if (flags & H5Z_FLAG_REVERSE) {
    const uint8_t *in = (const uint8_t *)*buf;
    uint32_t uncomp_len = 0, block = 0;
    size_t start = 0;

    /* snappy-cuda's layout.  A chunk from an older hdf5lib that happens to
       parse as one but does not decode as one falls through to the raw path. */
    if (is_blocked(in, nbytes, &uncomp_len, &block, &start)) {
      void *outbuf = H5allocate_memory(uncomp_len, 0);
      if (!outbuf) PUSH_ERR("snappy_filter: Memory allocation failed");

      if (decode_blocks(in, start, uncomp_len, block, (char *)outbuf)) {
        H5free_memory(*buf);
        *buf = outbuf;
        *buf_size = (size_t)uncomp_len;
        return (size_t)uncomp_len;
      }
      H5free_memory(outbuf);
    }

    /* A raw Snappy stream, as hdf5lib 2.2.0.0 and earlier wrote.  Snappy
       embeds the uncompressed length in the compressed stream header */
    int hdr_len = csnappy_get_uncompressed_length((const char *)*buf, (uint32_t)nbytes, &uncomp_len);
    if (hdr_len < 0) {
      PUSH_ERR("snappy_filter: Failed to parse uncompressed length from Snappy header");
    }

    void *outbuf = H5allocate_memory(uncomp_len, 0);
    if (!outbuf) PUSH_ERR("snappy_filter: Memory allocation failed");

    /* csnappy_decompress() would report success for a stream that ends early,
       leaving the rest of the buffer unset, so compare the length it produced */
    uint32_t produced = uncomp_len;
    int status = CSNAPPY_E_DATA_MALFORMED;
    if (elements_fit(in + hdr_len, nbytes - (size_t)hdr_len)) {
      status = csnappy_decompress_noheader((const char *)*buf + hdr_len, (uint32_t)nbytes - (uint32_t)hdr_len,
                                           (char *)outbuf, &produced);
    }
    if (status != CSNAPPY_E_OK || produced != uncomp_len) {
      H5free_memory(outbuf);
      PUSH_ERR("snappy_filter: Snappy decompression failed (chunk truncated or corrupt)");
    }

    H5free_memory(*buf);
    *buf = outbuf;
    *buf_size = (size_t)uncomp_len;
    return (size_t)uncomp_len;
  }

  /* ----- Compression Path ----- */
  else {
    /* Room for the two varints, plus each block's length and its worst case
       (32 + n + n/6).  snappy-cuda keeps lengths in 32 bits, so refuse a chunk
       whose output might not fit them. */
    size_t full = nbytes / SNAPPY_BLOCK_SIZE, rest = nbytes % SNAPPY_BLOCK_SIZE;
    uint64_t limit = 10 + (uint64_t)full * (4 + csnappy_max_compressed_length(SNAPPY_BLOCK_SIZE));
    if (rest) limit += 4 + csnappy_max_compressed_length((uint32_t)rest);
    if (limit > UINT32_MAX) {
      PUSH_ERR("snappy_filter: Chunk of %zu bytes is too large for Snappy", nbytes);
    }

    uint8_t *outbuf = H5allocate_memory((size_t)limit, 0);
    if (!outbuf) PUSH_ERR("snappy_filter: Memory allocation failed");

    /* csnappy requires a dedicated working memory buffer for compression */
    void *workmem = H5allocate_memory(1 << SNAPPY_WORKMEM_POWER, 0);
    if (!workmem) {
        H5free_memory(outbuf);
        PUSH_ERR("snappy_filter: Working memory allocation failed");
    }

    size_t pos = write_varint32(outbuf, (uint32_t)nbytes);
    pos += write_varint32(outbuf + pos, SNAPPY_BLOCK_SIZE);

    const char *in = (const char *)*buf;
    for (size_t done = 0; done < nbytes; ) {
      uint32_t len = (nbytes - done < SNAPPY_BLOCK_SIZE) ? (uint32_t)(nbytes - done) : SNAPPY_BLOCK_SIZE;

      /* A short block gets a smaller hash table, as csnappy_compress() does */
      int power = 9;
      while (power < SNAPPY_WORKMEM_POWER && (1u << (power - 1)) < len) power++;

      char *elements = (char *)outbuf + pos + 4;
      char *end = csnappy_compress_fragment(in + done, len, elements, workmem, power);
      write_le32(outbuf + pos, (uint32_t)(end - elements));
      pos += 4 + (size_t)(end - elements);
      done += len;
    }

    H5free_memory(workmem);

    H5free_memory(*buf);
    *buf = outbuf;
    *buf_size = (size_t)limit; /* HDF5 tracks the allocated buffer size */
    return pos;
  }
}

/* Register the filter class */
const H5Z_class2_t snappy_class = {
  H5Z_CLASS_T_VERS,
  H5Z_FILTER_SNAPPY,
  1,
  1,
  "snappy",
  NULL,  /* can_apply */
  NULL,  /* set_local (Not needed for Snappy) */
  snappy_filter
};
