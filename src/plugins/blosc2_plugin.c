/**
 * @file blosc2_plugin.c
 * @brief Standalone HDF5 Filter Plugin for Blosc2 (Filter ID 32026)
 * Supports Bitmask Pre-Filter Pipelines while maintaining 100% 
 * forward and backward compatibility with h5py and community plugins.
 * * Includes ZFP Safety Overrides to prevent bit-corruption from pre-filters.
 */

#include <hdf5.h>
#include <blosc2.h>
#include <blosc2/codecs-registry.h>
#include <b2nd.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#ifndef _WIN32
#include <unistd.h>
#endif
#include "hdf5lib.h"

#define FILTER_BLOSC2 32026
#define DEFAULT_CLEVEL 5
/* The compression level recorded for a dataset a ZFP codec cannot take (see
   blosc2_set_local()).  c-blosc2 rejects any level above 9, so every Blosc2
   filter, the reference one included, refuses each chunk. */
#define DECLINED_CLEVEL 255
#define DEFAULT_SHUFFLE 1
#define DEFAULT_COMPCODE BLOSC_BLOSCLZ

/* Max possible size: Base(8) + MaxDims(B2ND_MAX_DIM) + Meta(1), the last
   only in lists earlier versions stored (see CD_VALUES below).
   This used to read BLOSC2_MAX_DIM, which c-blosc2 3.x removed.  That macro was
   a stale duplicate: b2nd raised its own limit from 8 to 16 in c-blosc2 2.21.3
   while BLOSC2_MAX_DIM stayed at 8, so this array was half the size b2nd could
   actually produce.  The reference filter (Blosc/HDF5-Blosc2) tracks
   B2ND_MAX_DIM here, so we do too. */
#define MAX_FILTER_VALUES (8 + B2ND_MAX_DIM + 1)

#ifdef WORDS_BIGENDIAN
  #define B2ND_ENDIAN_PREFIX ">"
#else
  #define B2ND_ENDIAN_PREFIX "<"
#endif

#define B2ND_OPAQUE_NPDTYPE_FORMAT "|V%zd"
#define B2ND_OPAQUE_NPDTYPE_MAXLEN (2 + 20 + 1)

/* Blosc2 plugin codecs that need `compcode_meta` and the B2ND layer.  ZFP takes
   its accuracy/precision/rate there; NDLZ takes its cell size (4 or 8). */
#define IS_ZFP_CODEC(c) ((c) >= BLOSC_CODEC_ZFP_FIXED_ACCURACY && \
                         (c) <= BLOSC_CODEC_ZFP_FIXED_RATE)
#define NDLZ_DEFAULT_CELLSIZE 4

#define PUSH_ERR(...) do { \
    H5Epush(H5E_DEFAULT, __FILE__, __func__, __LINE__, H5E_ERR_CLS, H5E_PLINE, H5E_CANTFILTER, __VA_ARGS__); \
    return 0; \
} while(0)

/* =========================================================================
 * CD_VALUES
 * =========================================================================
 * Every slot holds what the reference filter (Blosc/HDF5-Blosc2, which h5py's
 * hdf5plugin builds) keeps there, so that it can append to hdf5lib's datasets
 * and hdf5plugin can describe them:
 *
 *   [0] revision (below)        [4] compression level, 0-9
 *   [1] block size, 0 for auto  [5] pre-filter, as one Blosc2 filter code
 *   [2] item size               [6] codec
 *   [3] chunk size in bytes     [7], [8...] chunk rank and shape (B2ND chunks)
 *
 * The reference records its revision, 1, in slot 0 and never reads it back.
 * hdf5lib packs there what no other slot can hold:
 *
 *   bits 31-24  0xB2, hdf5lib's tag
 *   bits 23-20  layout version, 1
 *   bits 19-16  pre-filters: 1 shuffle, 2 bitshuffle, 4 delta, 8 trunc-prec
 *   bits 15-8   meta value: trunc-prec's precision (a signed byte), NDLZ's
 *               cell size (4 or 8), or the ZFP codecs' parameter
 *   bits  7-0   1, the reference's revision
 *
 * The pre-filters run in the order trunc-prec, delta, bitshuffle or shuffle,
 * and slot 5 names the last of them: the one stage the reference runs.
 * Lists hdf5lib did not write this way are read by their slot 0:
 *
 *   0  a caller's, in hdf5lib's earlier form: slot 5 the pre-filter
 *      bitmask, and an 8th value the meta value
 *   1  the reference's: slot 5 a single filter code, and no meta value
 *   2  as earlier versions of hdf5lib stored them: slot 5's low byte the
 *      bitmask; the meta value in its next byte (7 values) or after the
 *      chunk shape */
#define REFERENCE_REVISION 1
#define LEGACY_REVISION    2
#define SETTINGS_TAG       0xB2u
#define SETTINGS_LAYOUT    1u
#define PACK_SETTINGS(mask, meta) ((SETTINGS_TAG << 24) | (SETTINGS_LAYOUT << 20) | \
                                   (((mask) & 0xFu) << 16) | (((meta) & 0xFFu) << 8) | \
                                   REFERENCE_REVISION)
#define IS_PACKED(v)     (((v) >> 24) == SETTINGS_TAG)
#define PACKED_LAYOUT(v) (((v) >> 20) & 0xFu)
#define PACKED_MASK(v)   (((v) >> 16) & 0xFu)
#define PACKED_META(v)   (((v) >> 8) & 0xFFu)

/* The filter code of the last pre-filter in `mask`, which slot 5 records */
static unsigned int last_prefilter(unsigned int mask) {
  if (mask & 2) return BLOSC_BITSHUFFLE;
  if (mask & 1) return BLOSC_SHUFFLE;
  if (mask & 4) return BLOSC_DELTA;
  if (mask & 8) return BLOSC_TRUNC_PREC;
  return BLOSC_NOSHUFFLE;
}

/* The bitmask for a filter code in a reference list's slot 5, or -1 for a
   filter hdf5lib does not run */
static int prefilter_mask(unsigned int code) {
  switch (code) {
    case BLOSC_NOSHUFFLE:  return 0;
    case BLOSC_SHUFFLE:    return 1;
    case BLOSC_BITSHUFFLE: return 2;
    case BLOSC_DELTA:      return 4;
    case BLOSC_TRUNC_PREC: return 8;
    default:               return -1;
  }
}

/* =========================================================================
 * PROPERTY LIST SETTER (C API, declared in hdf5lib.h)
 * ========================================================================= */
herr_t hdf5lib_set_blosc2(hid_t plist, unsigned int flags, unsigned int codec,
                          unsigned int level, unsigned int prefilters, int meta) {
  if (H5Pisa_class(plist, H5P_DATASET_CREATE) <= 0) {
    H5Epush(H5E_DEFAULT, __FILE__, __func__, __LINE__, H5E_ERR_CLS, H5E_PLIST, H5E_BADTYPE,
            "hdf5lib_set_blosc2: not a dataset creation property list");
    return -1;
  }
  /* Slot 0 has four bits for the pre-filters and a byte for the meta value;
     refuse what would not fit rather than store something else */
  if (level > 9 || prefilters > 0xF || meta < -128 || meta > 255) {
    H5Epush(H5E_DEFAULT, __FILE__, __func__, __LINE__, H5E_ERR_CLS, H5E_PLIST, H5E_BADVALUE,
            "hdf5lib_set_blosc2: level must be 0 to 9, prefilters 0 to 15 and meta -128 to 255, "
            "not %u, %u and %d", level, prefilters, meta);
    return -1;
  }

  unsigned int cd_values[7] = {PACK_SETTINGS(prefilters, (unsigned int)meta), 0, 0, 0,
                               level, last_prefilter(prefilters), codec};

  /* H5Pset_filter() would add a second Blosc2 filter; update the first instead */
  int nfilters = H5Pget_nfilters(plist);
  if (nfilters < 0) return -1;
  for (int i = 0; i < nfilters; i++) {
    unsigned int old_flags, old_values[1];
    size_t old_nelmts = 0;
    if (H5Pget_filter2(plist, (unsigned int)i, &old_flags, &old_nelmts, old_values, 0, NULL, NULL) == FILTER_BLOSC2)
      return H5Pmodify_filter(plist, FILTER_BLOSC2, flags, 7, cd_values);
  }
  return H5Pset_filter(plist, FILTER_BLOSC2, flags, 7, cd_values);
}

/* =========================================================================
 * CONTEXT CACHE
 * =========================================================================
 * c-blosc2 starts a context's worker threads the first time the context does
 * any work and joins them when the context is freed.  Building a context per
 * chunk therefore starts and stops the whole pool for every chunk, which with
 * chunks of a megabyte or so costs more than the threads save, and also
 * throws away each worker's codec state (a Zstd context, for one).  Instead,
 * keep one compression and one decompression context alive across filter
 * calls and lend them to each chunk's super-chunk in place of its own.
 *
 * HDF5 makes every filter call under its global lock (the bundled library is
 * built thread-safe), so the cache needs no lock of its own. */

/* Points a context at the super-chunk it is working for; codecs such as ZFP
   and NDLZ read their geometry from it (src/config/blosc2/blosc2_ctx_schunk.c). */
extern void hdf5lib_blosc2_ctx_set_schunk(blosc2_context *ctx, blosc2_schunk *schunk);

typedef struct {
  blosc2_context *ctx;
  blosc2_cparams  cparams;   /* what a compression context was created from */
  int32_t         nbytes;    /* chunk size its automatic blocksize was tuned for */
  int16_t         nthreads;
#ifndef _WIN32
  pid_t           pid;       /* process holding its worker threads, or 0 if none */
#endif
} ctx_cache_t;

static ctx_cache_t cctx_cache = {0};
static ctx_cache_t dctx_cache = {0};

#ifndef _WIN32
/* A forked child (parallel::mclapply(), for one) inherits the cached contexts
   but none of their worker threads, while c-blosc2 still lists the dead pool
   under its thread count, ready to hand to the next context that asks for that
   many threads.  So a child abandons the inherited contexts -- freeing them
   would wait on threads that no longer exist -- and runs single-threaded. */
static int inherited_dead_pool = 0;

static void drop_if_forked(ctx_cache_t *cache) {
  if (cache->ctx && cache->pid != 0 && cache->pid != getpid()) {
    memset(cache, 0, sizeof(*cache));
    inherited_dead_pool = 1;
  }
}
#endif

static int16_t blosc2_nthreads(void) {
#ifndef _WIN32
  drop_if_forked(&cctx_cache);
  drop_if_forked(&dctx_cache);
  if (inherited_dead_pool) return 1;
#endif
  return (int16_t)hdf5lib_get_blosc_nthreads();
}

static void cache_store(ctx_cache_t *cache, blosc2_context *ctx, int16_t nthreads) {
  cache->ctx      = ctx;
  cache->nthreads = nthreads;
#ifndef _WIN32
  cache->pid      = (nthreads > 1) ? getpid() : 0;
#endif
}

static void cache_free(ctx_cache_t *cache) {
  if (cache->ctx) blosc2_free_ctx(cache->ctx);
  memset(cache, 0, sizeof(*cache));
}

static int same_cparams(const blosc2_cparams *a, const blosc2_cparams *b) {
  return a->compcode == b->compcode && a->compcode_meta == b->compcode_meta &&
         a->clevel == b->clevel && a->use_dict == b->use_dict &&
         a->typesize == b->typesize && a->blocksize == b->blocksize &&
         a->splitmode == b->splitmode &&
         memcmp(a->filters, b->filters, sizeof(a->filters)) == 0 &&
         memcmp(a->filters_meta, b->filters_meta, sizeof(a->filters_meta)) == 0;
}

/* The cached compression context, rebuilt whenever the parameters change.  A
   compression context settles on its automatic blocksize the first time it
   runs and keeps it, so it is only reused for chunks of the same size. */
static blosc2_context *get_cctx(blosc2_context *template_ctx, int32_t nbytes) {
  /* blosc2_ctx_get_cparams() leaves some fields alone; keep them at defaults. */
  blosc2_cparams cparams = BLOSC2_CPARAMS_DEFAULTS;
  int16_t nthreads = blosc2_nthreads();

  if (blosc2_ctx_get_cparams(template_ctx, &cparams) < 0) return NULL;
  cparams.schunk   = NULL;
  cparams.nthreads = nthreads;

  if (cctx_cache.ctx && cctx_cache.nthreads == nthreads &&
      cctx_cache.nbytes == nbytes && same_cparams(&cctx_cache.cparams, &cparams))
    return cctx_cache.ctx;

  cache_free(&cctx_cache);
  blosc2_context *ctx = blosc2_create_cctx(cparams);
  if (!ctx) return NULL;
  cache_store(&cctx_cache, ctx, nthreads);
  cctx_cache.cparams = cparams;
  cctx_cache.nbytes  = nbytes;
  return ctx;
}

/* A decompression context takes everything else from the chunk it is given. */
static blosc2_context *get_dctx(void) {
  int16_t nthreads = blosc2_nthreads();

  if (dctx_cache.ctx && dctx_cache.nthreads == nthreads) return dctx_cache.ctx;

  cache_free(&dctx_cache);
  blosc2_dparams dparams = BLOSC2_DPARAMS_DEFAULTS;
  dparams.nthreads = nthreads;
  blosc2_context *ctx = blosc2_create_dctx(dparams);
  if (!ctx) return NULL;
  cache_store(&dctx_cache, ctx, nthreads);
  return ctx;
}

/* Lend the cached contexts to `schunk`, keeping its own to hand back before it
   is serialised or freed.  Returns -1 if a context could not be made. */
typedef struct {
  blosc2_context *cctx;
  blosc2_context *dctx;
} own_ctxs_t;

static int lend_contexts(blosc2_schunk *schunk, int32_t nbytes, own_ctxs_t *own) {
  blosc2_context *cctx = (nbytes > 0) ? get_cctx(schunk->cctx, nbytes) : NULL;
  blosc2_context *dctx = get_dctx();
  if ((nbytes > 0 && !cctx) || !dctx) return -1;

  own->cctx = schunk->cctx;
  own->dctx = schunk->dctx;
  if (cctx) {
    hdf5lib_blosc2_ctx_set_schunk(cctx, schunk);
    schunk->cctx = cctx;
  }
  hdf5lib_blosc2_ctx_set_schunk(dctx, schunk);
  schunk->dctx = dctx;
  return 0;
}

static void return_contexts(blosc2_schunk *schunk, const own_ctxs_t *own) {
  if (schunk->cctx != own->cctx) {
    hdf5lib_blosc2_ctx_set_schunk(schunk->cctx, NULL);
    schunk->cctx = own->cctx;
  }
  hdf5lib_blosc2_ctx_set_schunk(schunk->dctx, NULL);
  schunk->dctx = own->dctx;
}

/* Called by hdf5lib_destroy_all_filters() before blosc2_destroy(). */
void blosc2_plugin_free_contexts(void) {
#ifndef _WIN32
  drop_if_forked(&cctx_cache);
  drop_if_forked(&dctx_cache);
#endif
  cache_free(&cctx_cache);
  cache_free(&dctx_cache);
}

/* =========================================================================
 * NON-FINITE GUARD
 * =========================================================================
 * ZFP has no encoding for NaN or Inf: one turns into an arbitrary finite
 * number and spoils the other values in its block, too.  Refuse such a chunk
 * instead.  With H5Z_FLAG_OPTIONAL, HDF5 then stores that one chunk
 * unfiltered, so the data stays exact; with H5Z_FLAG_MANDATORY the write
 * fails rather than corrupting it. */
static int has_nonfinite(const void *buf, size_t nbytes, size_t typesize) {
  const unsigned char *p = (const unsigned char *)buf;
  if (typesize == 8) {
    for (size_t i = 0; i + 8 <= nbytes; i += 8) {
      uint64_t u;
      memcpy(&u, p + i, 8);
      if ((u & UINT64_C(0x7FF0000000000000)) == UINT64_C(0x7FF0000000000000)) return 1;
    }
  } else if (typesize == 4) {
    for (size_t i = 0; i + 4 <= nbytes; i += 4) {
      uint32_t u;
      memcpy(&u, p + i, 4);
      if ((u & UINT32_C(0x7F800000)) == UINT32_C(0x7F800000)) return 1;
    }
  }
  return 0;
}

/* A 4- or 8-byte float in native byte order: the items truncate-precision
   knows how to round, since the filter sees data in the file's byte order. */
static int is_native_float(hid_t type) {
  if (H5Tget_class(type) != H5T_FLOAT) return 0;
  size_t size = H5Tget_size(type);
  return (size == 4 || size == 8) && H5Tget_order(type) == H5Tget_order(H5T_NATIVE_DOUBLE);
}

/* =========================================================================
 * SET_LOCAL CALLBACK (METADATA INJECTION & PARSING)
 * ========================================================================= */

/* Reads the settings from a list handed to set_local: a caller's, or one a
   dataset's creation property list carried over from another dataset.  The
   chunk geometry after slot 6 is worked out afresh.  Returns -1, with the
   reason on HDF5's error stack, for a list it cannot read. */
static int read_settings(const unsigned int *values, size_t nelements, unsigned int *clevel,
                         unsigned int *mask, unsigned int *compcode, unsigned int *meta) {
  unsigned int revision = (nelements >= 1) ? values[0] : 0;
  *clevel   = (nelements >= 5) ? values[4] : DEFAULT_CLEVEL;
  *compcode = (nelements >= 7) ? values[6] : DEFAULT_COMPCODE;
  *meta     = 0;

  if (IS_PACKED(revision)) {
    if (PACKED_LAYOUT(revision) != SETTINGS_LAYOUT) {
      H5Epush(H5E_DEFAULT, __FILE__, __func__, __LINE__, H5E_ERR_CLS, H5E_PLINE, H5E_BADVALUE,
              "blosc2: cd_values[0] (0x%08X) is in settings layout %u; this hdf5lib reads layout %u",
              revision, PACKED_LAYOUT(revision), SETTINGS_LAYOUT);
      return -1;
    }
    *mask = PACKED_MASK(revision);
    *meta = PACKED_META(revision);
    /* Slot 5 may only restate the last stage of the pipeline slot 0 sets out */
    if (nelements >= 6 && values[5] != 0 && values[5] != last_prefilter(*mask)) {
      H5Epush(H5E_DEFAULT, __FILE__, __func__, __LINE__, H5E_ERR_CLS, H5E_PLINE, H5E_BADVALUE,
              "blosc2: cd_values[5] must be 0 or %u, the last pre-filter cd_values[0] selects, not %u",
              last_prefilter(*mask), values[5]);
      return -1;
    }
  } else if (revision == 0 || revision == LEGACY_REVISION) {
    /* The bitmask is in slot 5's low byte.  A caller gives the meta value as
       an 8th value; earlier versions stored it in slot 5's next byte (seven
       values) or last, after the chunk rank in slot 7 and the shape. */
    *mask = (nelements >= 6) ? (values[5] & 0xFF) : DEFAULT_SHUFFLE;
    *meta = (nelements >= 8) ? values[7]
          : (nelements >= 6) ? (values[5] >> 8) & 0xFF : 0;
    if (nelements >= 9 && nelements <= MAX_FILTER_VALUES && values[7] <= B2ND_MAX_DIM &&
        nelements == 9 + (size_t)values[7])
      *meta = values[nelements - 1];
  } else if (revision == REFERENCE_REVISION) {
    /* One filter code, and nowhere to keep a meta value */
    unsigned int code = (nelements >= 6) ? values[5] : DEFAULT_SHUFFLE;
    int code_mask = prefilter_mask(code);
    if (code_mask < 0) {
      H5Epush(H5E_DEFAULT, __FILE__, __func__, __LINE__, H5E_ERR_CLS, H5E_PLINE, H5E_BADVALUE,
              "blosc2: cd_values[5] must be a pre-filter code from 0 to 4 when cd_values[0] is 1, not %u",
              code);
      return -1;
    }
    *mask = (unsigned int)code_mask;
  } else {
    H5Epush(H5E_DEFAULT, __FILE__, __func__, __LINE__, H5E_ERR_CLS, H5E_PLINE, H5E_BADVALUE,
            "blosc2: cd_values[0] must be hdf5lib's packed settings (0xB21xxx01), 0, 1 or 2, not %u",
            revision);
    return -1;
  }

  *mask &= 0xF;
  *meta &= 0xFF;  /* c-blosc2 keeps only a byte anyway */
  return 0;
}

static herr_t blosc2_set_local(hid_t dcpl, hid_t type, hid_t space) {
  int ndim;
  herr_t r;
  unsigned int typesize, basetypesize, bufsize;
  hsize_t chunkshape[H5S_MAX_RANK];
  unsigned int flags;

  size_t nelements = MAX_FILTER_VALUES;
  unsigned int values[MAX_FILTER_VALUES];
  memset(values, 0, sizeof(values));

  r = H5Pget_filter_by_id(dcpl, FILTER_BLOSC2, &flags, &nelements, values, 0, NULL, NULL);
  if (r < 0) return -1;

  /* 1. Read the caller's settings, in any of the forms CD_VALUES describes */
  unsigned int user_clevel, user_filter_mask, user_compcode, user_meta;
  if (read_settings(values, nelements, &user_clevel, &user_filter_mask, &user_compcode, &user_meta) < 0)
    return -1;

  /* 2. Calculate Data Geometry */
  ndim = H5Pget_chunk(dcpl, H5S_MAX_RANK, chunkshape);
  if (ndim < 0) return -1;

  typesize = (unsigned int)H5Tget_size(type);
  if (typesize == 0) return -1;

  H5T_class_t classt = H5Tget_class(type);
  int float_items;
  if (classt == H5T_ARRAY) {
    hid_t super_type = H5Tget_super(type);
    if (super_type < 0) return -1;
    basetypesize = (unsigned int)H5Tget_size(super_type);
    float_items = is_native_float(super_type);
    H5Tclose(super_type);
  } else {
    basetypesize = typesize;
    float_items = is_native_float(type);
  }

  bufsize = typesize;
  for (int i = 0; i < ndim; i++) bufsize *= (unsigned int)chunkshape[i];

  /* Reject a configuration Blosc2 cannot honour *here*, while H5Dcreate2() can
     still report it.  A mandatory filter that only fails once a chunk is
     compressed fails inside H5Dclose(), where HDF5 discards the chunk and most
     callers never look at the return value: the data is lost silently. */
  const char *compname = NULL;
  if (blosc2_compcode_to_compname((int)user_compcode, &compname) < 0) return -1;

  /* DECLINED_CLEVEL comes from a list this function wrote for another
     dataset; that dataset's own level is gone, so fall back to the default. */
  if (user_clevel == DECLINED_CLEVEL) user_clevel = DEFAULT_CLEVEL;
  if (user_clevel > 9) {
    H5Epush(H5E_DEFAULT, __FILE__, __func__, __LINE__, H5E_ERR_CLS, H5E_PLINE, H5E_BADVALUE,
            "blosc2: compression level must be 0 to 9, not %u", user_clevel);
    return -1;
  }

  /* The ZFP codecs encode the bits of an IEEE float.  Handed integers, or
     floats in the other byte order, they compress them as native floats all
     the same, and the values read back as garbage (small integers as 0).  So
     a mandatory filter refuses such a dataset here, and an optional one
     records DECLINED_CLEVEL, which makes the filter refuse every chunk and
     HDF5 store each one unfiltered.  A chunk of fewer than 4 values cannot
     fill a ZFP block, so it is treated the same way. */
  if (IS_ZFP_CODEC(user_compcode) && (!float_items || bufsize / basetypesize < 4)) {
    if (!(flags & H5Z_FLAG_OPTIONAL)) {
      H5Epush(H5E_DEFAULT, __FILE__, __func__, __LINE__, H5E_ERR_CLS, H5E_PLINE, H5E_CANAPPLY,
              "blosc2: the ZFP codecs need float32 or float64 data in native byte order, "
              "at least 4 values a chunk");
      return -1;
    }
    user_clevel = DECLINED_CLEVEL;
  }

  /* Truncate-precision zeroes the low bits of every 4- or 8-byte item as
     though it were a float's mantissa, whatever the item holds: it would
     wipe out small integers, corrupt compound records and floats in the other
     byte order, and fail the chunk for any other item size.  So it only runs
     on native float32 and float64 values (alone or in arrays); for other data
     the flag is dropped, which lets one pipeline serve a whole file. */
  if (!float_items) user_filter_mask &= ~8u;

  /* Blosc2 reads the precision as a signed byte: n > 0 keeps n bits of the
     mantissa and n < 0 drops -n.  It refuses to clear the whole mantissa, and
     then fails every chunk, so refuse that here.  Keeping as many bits as the
     mantissa holds, or more, keeps them all.  The ZFP codecs and NDLZ skip the
     pre-filters, and use the same byte for parameters of their own. */
  if ((user_filter_mask & 8) && !IS_ZFP_CODEC(user_compcode) && user_compcode != BLOSC_CODEC_NDLZ) {
    int mantissa = (basetypesize == 4) ? 23 : 52;
    int prec = (int)(user_meta & 0xFF);
    if (prec > 127) prec -= 256;
    if (prec >= mantissa) {
      user_filter_mask &= ~8u;
    } else if (prec == 0 || prec <= -mantissa) {
      H5Epush(H5E_DEFAULT, __FILE__, __func__, __LINE__, H5E_ERR_CLS, H5E_PLINE, H5E_BADVALUE,
              "blosc2: truncate-precision must keep 1 to %d bits of a %u-byte float (or drop 1 to %d), not %d",
              mantissa, basetypesize, mantissa - 1, prec);
      return -1;
    }
  }

  /* Record only what runs.  The ZFP codecs and NDLZ skip the pre-filters,
     bitshuffle takes the place of shuffle, and the meta value stays only
     where something reads it. */
  if (IS_ZFP_CODEC(user_compcode) || user_compcode == BLOSC_CODEC_NDLZ) user_filter_mask = 0;
  if (user_filter_mask & 2) user_filter_mask &= ~1u;
  if (user_compcode == BLOSC_CODEC_NDLZ)
    user_meta = (user_meta == 8) ? 8 : NDLZ_DEFAULT_CELLSIZE;
  else if (!IS_ZFP_CODEC(user_compcode) && !(user_filter_mask & 8))
    user_meta = 0;

  /* B2ND geometry must describe the chunk's bytes.  For an array datatype the
     items Blosc2 sees are the array's base elements, which the chunk's
     dimensions do not count, so such chunks go without it (NDLZ excepted: it
     is handed the chunk's byte image below). */
  int geometry_fits = (user_compcode == BLOSC_CODEC_NDLZ) || typesize == basetypesize;

  if (user_compcode == BLOSC_CODEC_NDLZ) {
    /* NDLZ reads its 4x4 / 8x8 cell grid straight out of the B2ND block shape,
       so it can only see a 2-D array of single-byte items.  Rather than refuse
       every other chunk, hand it the chunk's raw byte image: the leading axes
       collapse into rows and the trailing axis widens by the item size.  The
       bytes keep their original order, so this is a relabelling rather than a
       transform, and B2ND records the shape in the frame for the read path. */
    hsize_t rows, cols;
    /* The full item size: an array datatype's items are whole arrays here */
    if (ndim == 1) {
      rows = chunkshape[0];
      cols = typesize;
    } else {
      rows = 1;
      for (int i = 0; i < ndim - 1; i++) rows *= chunkshape[i];
      cols = chunkshape[ndim - 1] * typesize;
    }
    ndim          = 2;
    chunkshape[0] = rows;
    chunkshape[1] = cols;
    basetypesize  = 1;
  }

  /* 3. Reconstruct cd_values in the reference filter's layout (see CD_VALUES) */
  values[0] = PACK_SETTINGS(user_filter_mask, user_meta);
  values[1] = 0; /* Blocksize: 0 tells Python/Blosc to calculate it automatically */
  values[2] = basetypesize;
  values[3] = bufsize;
  values[4] = user_clevel;
  values[5] = last_prefilter(user_filter_mask);
  values[6] = user_compcode;

  /* 4. Append ndim and shape starting precisely at index 7, and nothing after.
     The reference filter (h5py's hdf5plugin) reads a rank from slot 7 whenever
     there is one and refuses any below 2, so, like it, record B2ND geometry
     only for ranks 2..B2ND_MAX_DIM.  Other chunks -- 1-D ones, and ranks that
     cd_values cannot carry -- get exactly its seven values and use plain
     Blosc2 (or, for ZFP, 1-D B2ND). */
  int use_geometry = geometry_fits && ndim >= 2 && ndim <= B2ND_MAX_DIM;

  /* c-blosc2's ZFP codecs take B2ND ranks of 4 at most, and refuse blocks
     narrower than 4 along any axis, which a chunk dimension under 4 gives.
     Such chunks use 1-D ZFP instead. */
  if (IS_ZFP_CODEC(user_compcode)) {
    if (ndim > 4) use_geometry = 0;
    for (int i = 0; i < ndim; i++)
      if (chunkshape[i] < 4) use_geometry = 0;
  }

  size_t idx = 7;
  if (use_geometry) {
      values[idx++] = (unsigned int)ndim;
      for (int i = 0; i < ndim; i++) {
          values[idx++] = (unsigned int)chunkshape[i];
      }
  }

  nelements = idx;
  if (H5Pmodify_filter(dcpl, FILTER_BLOSC2, flags, nelements, values) < 0) return -1;
  return 1;
}

/* =========================================================================
 * BLOCK SIZE HEURISTICS
 * ========================================================================= */
static int32_t compute_blosc2_blocksize(int32_t chunksize, int32_t typesize, int clevel, int compcode) {
  static uint8_t data_dest[BLOSC2_MAX_OVERHEAD];
  blosc2_cparams cparams = BLOSC2_CPARAMS_DEFAULTS;
  cparams.compcode = (compcode < 0) ? DEFAULT_COMPCODE : compcode;
  cparams.clevel = clevel;
  cparams.typesize = typesize;

  if (blosc2_chunk_zeros(cparams, chunksize, data_dest, BLOSC2_MAX_OVERHEAD) < 0) return -1;

  int32_t blocksize = -1;
  if (blosc2_cbuffer_sizes(data_dest, NULL, NULL, &blocksize) < 0) return -1;
  return blocksize;
}

static int32_t compute_b2nd_block_shape(size_t block_size, size_t type_size, const int rank, const int32_t *dims_chunk, int32_t *dims_block) {
  size_t nitems = block_size / type_size;
  size_t nitems_new = 1;
  for (int i = 0; i < rank; i++) {
    dims_block[i] = dims_chunk[i] == 1 ? 1 : 2;
    nitems_new *= dims_block[i];
  }

  if (nitems_new >= nitems) return (int32_t)(nitems_new * type_size);

  while (nitems_new < nitems) {
    size_t nitems_prev = nitems_new;
    for (int i = rank - 1; i >= 0; i--) {
      if (dims_block[i] * 2 <= dims_chunk[i]) {
        if (nitems_new * 2 <= nitems) {
          nitems_new *= 2;
          dims_block[i] *= 2;
        }
      } else if (dims_block[i] < dims_chunk[i]) {
        size_t newitems_ext = (nitems_new / dims_block[i]) * dims_chunk[i];
        if (newitems_ext <= nitems) {
          nitems_new = newitems_ext;
          dims_block[i] = dims_chunk[i];
        }
      }
    }
    if (nitems_new == nitems_prev) break; 
  }
  return (int32_t)(nitems_new * type_size);
}

/* =========================================================================
 * CORE FILTER IMPLEMENTATION
 * ========================================================================= */
static size_t blosc2_filter_function(
    unsigned int flags, size_t cd_nelmts, const unsigned int cd_values[], 
    size_t nbytes, size_t *buf_size, void **buf) {

  if (nbytes == 0) return 0;

  void *outbuf = NULL;
  int64_t status = 0;

  /* The reference filter reads with 4 values and fills in the rest, and its
     set_local leaves a 1-D chunk's list as short as the user gave it */
  if (cd_nelmts < 4) PUSH_ERR("blosc2_filter: Filter parameters corrupted");

  /* Match the strict Python layout indices exactly */
  size_t blocksize   = cd_values[1];
  size_t typesize    = cd_values[2];
  size_t outbuf_size = cd_values[3];
  int clevel         = (cd_nelmts > 4) ? (int)cd_values[4] : DEFAULT_CLEVEL;
  int compcode       = (cd_nelmts > 6) ? (int)cd_values[6] : DEFAULT_COMPCODE;
  
  int ndim = -1;
  int32_t chunkshape[B2ND_MAX_DIM];
  size_t idx = 7; 

  /* Only extract appended C geometry if the writer included it.  Files from
     hdf5lib 2.2.0.0 and earlier also record ranks 1 and 0 here. */
  if (cd_nelmts >= 8) {
      ndim = (int)cd_values[idx++];
      if (ndim < 0 || ndim > B2ND_MAX_DIM)
        PUSH_ERR("blosc2_filter: Invalid rank (%d) in filter parameters", ndim);
      if (cd_nelmts < idx + (size_t)ndim)
        PUSH_ERR("blosc2_filter: Filter parameters truncated");
      for (int i = 0; i < ndim; i++) {
          chunkshape[i] = (int32_t)cd_values[idx++];
      }
  }

  /* The geometry must account for exactly the bytes HDF5 hands over.  It does
     not when a filter that changes the size (scale-offset, or Fletcher32
     placed first) runs ahead of this one, or for datasets earlier versions
     wrote with an array datatype.  B2ND would then compress only as much of
     the chunk as the geometry covers, so fall back as the reference filter
     does: plain Blosc2, or 1-D ZFP.  NDLZ needs its 2-D shape and gives up. */
  if (ndim >= 1 && !(flags & H5Z_FLAG_REVERSE)) {
    size_t geometry_bytes = typesize;
    for (int i = 0; i < ndim; i++) geometry_bytes *= (size_t)chunkshape[i];
    if (geometry_bytes != nbytes) {
      if (compcode == BLOSC_CODEC_NDLZ)
        PUSH_ERR("blosc2_filter: Chunk of %zu bytes does not match its NDLZ geometry", nbytes);
      ndim = -1;
    }
  }

  /* ----- Compression Path ----- */
  if (!(flags & H5Z_FLAG_REVERSE)) {

    /* The pre-filters and meta value, by the list's slot 0 (see CD_VALUES).
       Reading needs neither: every chunk records its own. */
    int filter_mask, meta_value = 0;
    if (IS_PACKED(cd_values[0])) {
      if (PACKED_LAYOUT(cd_values[0]) != SETTINGS_LAYOUT)
        PUSH_ERR("blosc2_filter: Settings layout %u is from a newer hdf5lib", PACKED_LAYOUT(cd_values[0]));
      filter_mask = (int)PACKED_MASK(cd_values[0]);
      meta_value  = (int)PACKED_META(cd_values[0]);
    } else if (cd_values[0] == LEGACY_REVISION) {
      /* The meta value follows the geometry, or without any sits in bits
         8-15 of slot 5 */
      filter_mask = (cd_nelmts > 5) ? (int)(cd_values[5] & 0xFF) : DEFAULT_SHUFFLE;
      if (cd_nelmts > idx) meta_value = (int)cd_values[idx];
      else if (cd_nelmts > 5) meta_value = (int)((cd_values[5] >> 8) & 0xFF);
    } else {
      /* The reference filter's: one filter code, no meta value */
      unsigned int code = (cd_nelmts > 5) ? cd_values[5] : DEFAULT_SHUFFLE;
      filter_mask = prefilter_mask(code);
      if (filter_mask < 0) PUSH_ERR("blosc2_filter: Pre-filter %u is not one hdf5lib runs", code);
    }

    if (clevel == DECLINED_CLEVEL)
      PUSH_ERR("blosc2_filter: The ZFP codecs cannot compress this dataset's datatype");

    if (IS_ZFP_CODEC(compcode) && has_nonfinite(*buf, nbytes, typesize))
      PUSH_ERR("blosc2_filter: ZFP cannot encode the NaN or Inf values in this chunk");

    /* ZFP works through B2ND; a chunk recorded without geometry is one row */
    if (IS_ZFP_CODEC(compcode) && ndim < 1) {
      if (typesize == 0 || nbytes % typesize != 0 || nbytes / typesize > INT32_MAX)
        PUSH_ERR("blosc2_filter: Chunk of %zu bytes does not hold whole %zu-byte items", nbytes, typesize);
      ndim = 1;
      chunkshape[0] = (int32_t)(nbytes / typesize);
    }

    blosc2_cparams cparams = BLOSC2_CPARAMS_DEFAULTS;
    cparams.compcode = compcode;
    cparams.typesize = (int32_t)typesize;
    cparams.clevel   = clevel;

    if (IS_ZFP_CODEC(compcode)) {
        cparams.compcode_meta = meta_value;
    } else if (compcode == BLOSC_CODEC_NDLZ) {
        /* NDLZ reads its cell size here, and accepts only 4 or 8.  The meta slot
           is shared with the pre-filter metadata, so fall back to the default
           rather than handing NDLZ a value it will reject. */
        cparams.compcode_meta = (meta_value == 8) ? 8 : NDLZ_DEFAULT_CELLSIZE;
    } else {
        cparams.compcode_meta = 0;
    }

    for (int i = 0; i < BLOSC2_MAX_FILTERS; i++) {
        cparams.filters[i] = 0; 
        cparams.filters_meta[i] = 0;
    }

    /* ZFP strictly bypasses Bitmask pre-filters to prevent mantissa corruption.
       NDLZ sees the chunk as loose bytes, so the item-typed pre-filters have
       nothing to work with either -- trunc-prec only handles 4- and 8-byte items
       and fails the whole chunk if it is handed anything else. */
    if (!IS_ZFP_CODEC(compcode) && compcode != BLOSC_CODEC_NDLZ) {
        int f_idx = 0;
        
        if (filter_mask & 8) {
            cparams.filters[f_idx] = BLOSC_TRUNC_PREC;
            cparams.filters_meta[f_idx] = meta_value; 
            f_idx++;
        }
        
        if (filter_mask & 4) {
            cparams.filters[f_idx] = BLOSC_DELTA;
            f_idx++;
        }
        
        if (filter_mask & 2) {
            cparams.filters[f_idx] = BLOSC_BITSHUFFLE;
            f_idx++;
        } else if (filter_mask & 1) {
            cparams.filters[f_idx] = BLOSC_SHUFFLE;
            f_idx++;
        }
    }

    blosc2_storage storage = {.cparams = &cparams, .contiguous = false};

    /* Multi-Dimensional (B2ND) Chunking */
    if (ndim >= 1 && (ndim > 1 || IS_ZFP_CODEC(compcode))) {
      b2nd_context_t *ctx = NULL;
      b2nd_array_t *array = NULL;

      if (blocksize == 0) {
        int32_t sugg = compute_blosc2_blocksize((int32_t)outbuf_size, (int32_t)typesize, cparams.clevel, compcode);
        if (sugg < 0) PUSH_ERR("blosc2_filter: Failed to compute suggested blocksize");
        blocksize = sugg;
      }
      
      int32_t blockdims[B2ND_MAX_DIM];
      cparams.blocksize = compute_b2nd_block_shape(blocksize, typesize, ndim, chunkshape, blockdims);

      int64_t chunkshape_l[B2ND_MAX_DIM];
      for (int i = 0; i < ndim; i++) chunkshape_l[i] = chunkshape[i];

      char dtype[B2ND_OPAQUE_NPDTYPE_MAXLEN];

      if (IS_ZFP_CODEC(compcode)) {
          snprintf(dtype, sizeof(dtype), "%sf%u", B2ND_ENDIAN_PREFIX, (unsigned int)typesize);
      } else {
          snprintf(dtype, sizeof(dtype), B2ND_OPAQUE_NPDTYPE_FORMAT, (size_t)typesize);
      }
      
      if (!(ctx = b2nd_create_ctx(&storage, ndim, chunkshape_l, chunkshape, blockdims, dtype, DTYPE_NUMPY_FORMAT, NULL, 0))) {
        PUSH_ERR("blosc2_filter: Cannot create B2ND context");
      }

      /* What b2nd_from_cbuffer() does, but compressing with the cached contexts */
      if (b2nd_empty(ctx, &array) < 0) {
        b2nd_free_ctx(ctx);
        PUSH_ERR("blosc2_filter: Cannot create B2ND array");
      }

      own_ctxs_t own;
      if (lend_contexts(array->sc, (int32_t)nbytes, &own) < 0) {
        b2nd_free(array); b2nd_free_ctx(ctx);
        PUSH_ERR("blosc2_filter: Cannot create Blosc2 contexts");
      }

      int64_t start[B2ND_MAX_DIM] = {0};
      int rc = b2nd_set_slice_cbuffer(*buf, array->shape, (int64_t)nbytes, start, array->shape, array);
      return_contexts(array->sc, &own);

      if (rc < 0) {
        b2nd_free(array); b2nd_free_ctx(ctx);
        PUSH_ERR("blosc2_filter: Cannot compress buffer into B2ND array");
      }

      bool needs_free = false;
      uint8_t *tmp_out = NULL;
      if (b2nd_to_cframe(array, &tmp_out, &status, &needs_free) < 0) {
        b2nd_free(array); b2nd_free_ctx(ctx);
        PUSH_ERR("blosc2_filter: Cannot convert B2ND array to buffer");
      }

      if (status <= 0) {
          if (needs_free && tmp_out) free(tmp_out);
          b2nd_free(array); 
          b2nd_free_ctx(ctx);
          PUSH_ERR("blosc2_filter: B2ND compression failed");
      }

      outbuf = H5allocate_memory((size_t)status, 0);
      memcpy(outbuf, tmp_out, (size_t)status);
      
      if (needs_free && tmp_out) free(tmp_out);
      b2nd_free(array);
      b2nd_free_ctx(ctx);
    } 
    /* 1D Linear Chunking */
    else {
      cparams.blocksize = (int32_t)blocksize;

      blosc2_schunk *schunk = blosc2_schunk_new(&storage);
      if (!schunk) PUSH_ERR("blosc2_filter: Cannot create super-chunk");

      own_ctxs_t own;
      if (lend_contexts(schunk, (int32_t)nbytes, &own) < 0) {
        blosc2_schunk_free(schunk);
        PUSH_ERR("blosc2_filter: Cannot create Blosc2 contexts");
      }

      int64_t rc = blosc2_schunk_append_buffer(schunk, *buf, (int32_t)nbytes);
      return_contexts(schunk, &own);

      if (rc < 0) {
        blosc2_schunk_free(schunk);
        PUSH_ERR("blosc2_filter: Cannot append buffer to super-chunk");
      }

      bool needs_free = false;
      uint8_t *tmp_out = NULL;
      status = blosc2_schunk_to_buffer(schunk, &tmp_out, &needs_free);
      
      if (status <= 0) {
          if (needs_free && tmp_out) free(tmp_out);
          blosc2_schunk_free(schunk); 
          PUSH_ERR("blosc2_filter: Super-chunk compression failed");
      }

      outbuf = H5allocate_memory((size_t)status, 0);
      memcpy(outbuf, tmp_out, (size_t)status);
      
      if (needs_free && tmp_out) free(tmp_out);
      blosc2_schunk_free(schunk);
    }
  } 
  
  /* ----- Decompression Path ----- */
  else {
    blosc2_schunk *schunk = blosc2_schunk_from_buffer(*buf, (int64_t)nbytes, false);
    if (!schunk) PUSH_ERR("blosc2_filter: Cannot get super-chunk from buffer");

    /* B2ND Array Decompression */
    if (blosc2_meta_exists(schunk, "b2nd") >= 0 || blosc2_meta_exists(schunk, "caterva") >= 0) {
      b2nd_array_t *array = NULL;

      if (b2nd_from_schunk(schunk, &array) < 0) {
        blosc2_schunk_free(schunk);
        PUSH_ERR("blosc2_filter: Cannot create B2ND array");
      }
      
      int64_t start[B2ND_MAX_DIM] = {0};
      int64_t stop[B2ND_MAX_DIM] = {0};
      
      /* Trust the B2ND header entirely. Ignore cd_values chunkshape perfectly. */
      int64_t size = schunk->typesize; 
      for (int i = 0; i < array->ndim; i++) {
        start[i] = 0;
        stop[i] = array->shape[i];
        size *= array->shape[i];
      }

      outbuf_size = (size_t)size;

      outbuf = H5allocate_memory(outbuf_size, 0);
      if (!outbuf) { 
        b2nd_free(array); 
        PUSH_ERR("blosc2_filter: Cannot allocate decompression buffer"); 
      }

      own_ctxs_t own;
      if (lend_contexts(array->sc, 0, &own) < 0) {
        H5free_memory(outbuf);
        b2nd_free(array);
        PUSH_ERR("blosc2_filter: Cannot create Blosc2 context");
      }

      int rc = b2nd_get_slice_cbuffer(array, start, stop, outbuf, stop, (int32_t)size);
      return_contexts(array->sc, &own);

      if (rc < 0) {
        H5free_memory(outbuf); 
        b2nd_free(array);
        PUSH_ERR("blosc2_filter: Cannot decompress B2ND array");
      }
      
      status = size;
      b2nd_free(array);
      schunk = NULL;
    } 
    /* 1D Linear Decompression */
    else {
      uint8_t *chunk = NULL;
      bool needs_free = false;
      int32_t cbytes = blosc2_schunk_get_lazychunk(schunk, 0, &chunk, &needs_free);
      
      if (cbytes < 0) { 
        blosc2_schunk_free(schunk); 
        PUSH_ERR("blosc2_filter: Cannot get chunk from super-chunk"); 
      }

      int32_t exact_bytes;
      blosc2_cbuffer_sizes(chunk, &exact_bytes, NULL, NULL);
      outbuf_size = (size_t)exact_bytes;

      outbuf = H5allocate_memory(outbuf_size, 0);
      if (!outbuf) { 
        if (needs_free && chunk) free(chunk);
        blosc2_schunk_free(schunk);
        PUSH_ERR("blosc2_filter: Cannot allocate decompression buffer"); 
      }

      own_ctxs_t own;
      if (lend_contexts(schunk, 0, &own) < 0) {
        H5free_memory(outbuf);
        if (needs_free && chunk) free(chunk);
        blosc2_schunk_free(schunk);
        PUSH_ERR("blosc2_filter: Cannot create Blosc2 context");
      }

      status = blosc2_decompress_ctx(schunk->dctx, chunk, cbytes, outbuf, (int32_t)outbuf_size);
      return_contexts(schunk, &own);

      if (needs_free && chunk) free(chunk);

      if (status <= 0) {
        H5free_memory(outbuf);
        blosc2_schunk_free(schunk);
        PUSH_ERR("blosc2_filter: Cannot decompress chunk into buffer");
      }
    }
    
    if (schunk) blosc2_schunk_free(schunk);
  }

  if (status > 0 && outbuf) {
    H5free_memory(*buf);
    *buf = outbuf;
    *buf_size = (flags & H5Z_FLAG_REVERSE) ? outbuf_size : (size_t)status;
    return (size_t)status;
  }

  if (outbuf) H5free_memory(outbuf);
  return 0;
}

const H5Z_class2_t blosc2_class = { 
  H5Z_CLASS_T_VERS, 
  FILTER_BLOSC2, 
  1, 1, 
  "blosc2", 
  NULL, 
  blosc2_set_local, 
  blosc2_filter_function 
};
