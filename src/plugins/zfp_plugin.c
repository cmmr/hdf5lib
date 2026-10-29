/**
 * @file zfp_plugin.c
 * @brief Standalone HDF5 Filter Plugin for ZFP
 * 
 * Consolidated from the official LLNL H5Z-ZFP repository.
 * Modified for R package integration: uses HDF5 allocators, strips Fortran/Silo, 
 * and modernizes error handling.
 */

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <hdf5.h>
#include <zfp.h>
#include "hdf5lib.h"

/* --- Versioning & Constants --- */
#define H5Z_FILTER_ZFP 32013

#define H5Z_FILTER_ZFP_VERSION_MAJOR 1
#define H5Z_FILTER_ZFP_VERSION_MINOR 1
#define H5Z_FILTER_ZFP_VERSION_PATCH 1

#define H5Z_ZFP_MODE_RATE      1
#define H5Z_ZFP_MODE_PRECISION 2
#define H5Z_ZFP_MODE_ACCURACY  3
#define H5Z_ZFP_MODE_EXPERT    4
#define H5Z_ZFP_MODE_REVERSIBLE 5

#define H5Z_ZFP_CD_NELMTS_MEM 6
#define H5Z_ZFP_CD_NELMTS_MAX 6

/* For a chunk ZFP cannot take, set_local() records the version word and one
   zero word where the ZFP header would go.  No ZFP header starts with a zero
   word, so this filter and the upstream H5Z-ZFP filter alike refuse every
   chunk, and HDF5 stores each one unfiltered. */
#define H5Z_ZFP_CD_NELMTS_DECLINE 2

#define ZFP_VERSION_NO__(Maj,Min,Pat,Twk)  (0x ## Maj ## Min ## Pat ## Twk)
#define ZFP_VERSION_NO_(Maj,Min,Pat,Twk)   ZFP_VERSION_NO__(Maj,Min,Pat,Twk)

#if defined(ZFP_VERSION_TWEAK)
#define ZFP_VERSION_NO ZFP_VERSION_NO_(ZFP_VERSION_MAJOR,ZFP_VERSION_MINOR,ZFP_VERSION_PATCH,ZFP_VERSION_TWEAK)
#elif defined(ZFP_VERSION_RELEASE)
#define ZFP_VERSION_NO ZFP_VERSION_NO_(ZFP_VERSION_MAJOR,ZFP_VERSION_MINOR,ZFP_VERSION_RELEASE,0)
#elif defined(ZFP_VERSION_PATCH)
#define ZFP_VERSION_NO ZFP_VERSION_NO_(ZFP_VERSION_MAJOR,ZFP_VERSION_MINOR,ZFP_VERSION_PATCH,0)
#else
#error ZFP LIBRARY VERSION NOT DETECTED
#endif

#ifndef ZFP_VERSION_STRING
#define ZFP_VERSION_STR__(Maj,Min,Rel) #Maj "." #Min "." #Rel
#define ZFP_VERSION_STR_(Maj,Min,Rel)  ZFP_VERSION_STR__(Maj,Min,Rel)
#define ZFP_VERSION_STRING             ZFP_VERSION_STR_(ZFP_VERSION_MAJOR,ZFP_VERSION_MINOR,ZFP_VERSION_RELEASE)
#endif

#ifndef ZFP_CODEC
#define ZFP_CODEC ZFP_VERSION_MINOR
#endif

#define H5Z_FILTER_ZFP_VERSION_NO__(Maj,Min,Pat)  (0x0 ## Maj ## Min ## Pat)
#define H5Z_FILTER_ZFP_VERSION_NO_(Maj,Min,Pat)   H5Z_FILTER_ZFP_VERSION_NO__(Maj,Min,Pat)
#define H5Z_FILTER_ZFP_VERSION_NO                 H5Z_FILTER_ZFP_VERSION_NO_(H5Z_FILTER_ZFP_VERSION_MAJOR,H5Z_FILTER_ZFP_VERSION_MINOR,H5Z_FILTER_ZFP_VERSION_PATCH)

#define PUSH_AND_GOTO(MINOR, RET, MSG) do { \
    H5Epush(H5E_DEFAULT, __FILE__, __func__, __LINE__, H5E_ERR_CLS, H5E_PLINE, MINOR, MSG); \
    retval = RET; \
    goto done; \
} while(0)

/* --- Internal Structures --- */
typedef struct _h5z_zfp_controls_t {
    unsigned int mode;
    union {
        double rate;
        double acc;
        unsigned int prec;
        struct expert_ {
            unsigned int minbits;
            unsigned int maxbits;
            unsigned int maxprec;
            int minexp;
        } expert;
    } details;
} h5z_zfp_controls_t;


/* =========================================================================
 *  PROPERTY LIST SETTERS (C API)
 * ========================================================================= */
static herr_t H5Pset_zfp(hid_t plist, int mode, ...) {
    static size_t const ctrls_sz = sizeof(h5z_zfp_controls_t);
    unsigned int flags;
    size_t cd_nelmts = 0;
    unsigned int cd_values[1];
    h5z_zfp_controls_t *ctrls_p = NULL;
    va_list ap;
    int ap_open = 0;  /* errors jump to done, which must still va_end() */
    herr_t retval = 0;

    if (0 >= H5Pisa_class(plist, H5P_DATASET_CREATE))
        PUSH_AND_GOTO(H5E_BADTYPE, -1, "not a dataset creation property list class");

    ctrls_p = (h5z_zfp_controls_t *) H5allocate_memory(ctrls_sz, 0);
    if (!ctrls_p) PUSH_AND_GOTO(H5E_NOSPACE, -1, "allocation failed for ZFP controls");

    va_start(ap, mode);
    ap_open = 1;
    ctrls_p->mode = mode;
    switch (mode) {
        case H5Z_ZFP_MODE_RATE:
            ctrls_p->details.rate = va_arg(ap, double);
            if (0 > ctrls_p->details.rate) PUSH_AND_GOTO(H5E_BADVALUE, -1, "rate out of range.");
            break;
        case H5Z_ZFP_MODE_ACCURACY:
            ctrls_p->details.acc = va_arg(ap, double);
            if (0 > ctrls_p->details.acc) PUSH_AND_GOTO(H5E_BADVALUE, -1, "accuracy out of range.");
            break;
        case H5Z_ZFP_MODE_PRECISION:
            ctrls_p->details.prec = va_arg(ap, unsigned int);
            break;
        case H5Z_ZFP_MODE_EXPERT:
            ctrls_p->details.expert.minbits = va_arg(ap, unsigned int);
            ctrls_p->details.expert.maxbits = va_arg(ap, unsigned int);
            ctrls_p->details.expert.maxprec = va_arg(ap, unsigned int);
            ctrls_p->details.expert.minexp  = va_arg(ap, int);
            break;
        case H5Z_ZFP_MODE_REVERSIBLE:
            break;
        default:
            PUSH_AND_GOTO(H5E_BADVALUE, -1, "bad ZFP mode.");
    }
    va_end(ap);
    ap_open = 0;

    for (int i = 0; i < H5Pget_nfilters(plist); i++) {
        H5Z_filter_t fid;
        if (0 <= (fid = H5Pget_filter(plist, i, &flags, &cd_nelmts, cd_values, 0, 0, 0)) && fid == H5Z_FILTER_ZFP) {
            if (0 > H5Premove_filter(plist, H5Z_FILTER_ZFP))
                PUSH_AND_GOTO(H5E_BADVALUE, -1, "Unable to remove old ZFP filter from pipeline.");
            break;
        }
    }

    if (0 > H5Pset_filter(plist, H5Z_FILTER_ZFP, H5Z_FLAG_MANDATORY, 0, 0))
        PUSH_AND_GOTO(H5E_BADVALUE, -1, "Unable to put ZFP filter in pipeline.");

    if (0 == H5Pexist(plist, "zfp_controls")) {
        retval = H5Pinsert2(plist, "zfp_controls", ctrls_sz, ctrls_p, 0, 0, 0, 0, 0, 0);
    } else {
        retval = H5Pset(plist, "zfp_controls", ctrls_p);
    }

done:
    if (ap_open) va_end(ap);
    if (ctrls_p) H5free_memory(ctrls_p);
    return retval;
}

herr_t H5Pset_zfp_rate(hid_t plist, double rate) { return H5Pset_zfp(plist, H5Z_ZFP_MODE_RATE, rate); }
herr_t H5Pset_zfp_precision(hid_t plist, unsigned int prec) { return H5Pset_zfp(plist, H5Z_ZFP_MODE_PRECISION, prec); }
herr_t H5Pset_zfp_accuracy(hid_t plist, double acc) { return H5Pset_zfp(plist, H5Z_ZFP_MODE_ACCURACY, acc); }
herr_t H5Pset_zfp_expert(hid_t plist, unsigned int minbits, unsigned int maxbits, unsigned int maxprec, int minexp) {
    return H5Pset_zfp(plist, H5Z_ZFP_MODE_EXPERT, minbits, maxbits, maxprec, minexp);
}
herr_t H5Pset_zfp_reversible(hid_t plist) { return H5Pset_zfp(plist, H5Z_ZFP_MODE_REVERSIBLE); }


/* =========================================================================
 *  CORE PLUGIN IMPLEMENTATION
 * ========================================================================= */

/* Whether ZFP can compress chunks of this datatype and shape: 4- or 8-byte
   floats (or, from ZFP 0.5.1, integers) in native byte order, with 1...3 (from
   ZFP 0.5.4, 1...4) dimensions longer than one.  Returns 1 and fills in the ZFP
   type and those dimensions if it can, 0 with *why saying why not, or -1 on
   error.  can_apply() and set_local() both ask here, so they cannot disagree. */
static htri_t zfp_chunk_supported(hid_t type_id, hid_t chunk_space_id, zfp_type *zt,
                                  int *ndims_used, hsize_t dims_used[], const char **why) {
    int const max_ndims = (ZFP_VERSION_NO >= 0x0540) ? 4 : 3;
    int ndims;
    size_t dsize;
    htri_t retval = 0;
    hsize_t dims[H5S_MAX_RANK];
    H5T_class_t dclass;
    hid_t native_type_id = H5I_INVALID_HID;

    *ndims_used = 0;
    *why = NULL;

    if (H5T_NO_CLASS == (dclass = H5Tget_class(type_id))) PUSH_AND_GOTO(H5E_BADTYPE, -1, "bad datatype class");
    if (0 == (dsize = H5Tget_size(type_id))) PUSH_AND_GOTO(H5E_BADTYPE, -1, "bad datatype size");
    if (0 > (ndims = H5Sget_simple_extent_dims(chunk_space_id, dims, 0))) PUSH_AND_GOTO(H5E_BADTYPE, -1, "bad chunk data space");

    if (dclass == H5T_FLOAT && dsize == sizeof(float)) *zt = zfp_type_float;
    else if (dclass == H5T_FLOAT && dsize == sizeof(double)) *zt = zfp_type_double;
#if ZFP_VERSION_NO >= 0x0510
    else if (dclass == H5T_INTEGER && dsize == sizeof(int32_t)) *zt = zfp_type_int32;
    else if (dclass == H5T_INTEGER && dsize == sizeof(int64_t)) *zt = zfp_type_int64;
    else { *why = "requires a datatype of class H5T_FLOAT or H5T_INTEGER and size 4 or 8"; goto done; }
#else
    else { *why = "requires a datatype of class H5T_FLOAT and size 4 or 8"; goto done; }
#endif

    for (int i = 0; i < ndims; i++) {
        if (dims[i] <= 1) continue;
        dims_used[(*ndims_used)++] = dims[i];
    }

    if (*ndims_used == 0 || *ndims_used > max_ndims) {
#if ZFP_VERSION_NO >= 0x0540
        *why = "chunk must have only 1...4 non-unity dimensions";
#else
        *why = "chunk must have only 1...3 non-unity dimensions";
#endif
        goto done;
    }

    /* ZFP's header packs the dimensions into 48 bits: 2^48 values in 1-D,
       down to 4096 per dimension in 4-D */
    {
        zfp_field *field = NULL;
        switch (*ndims_used) {
            case 1: field = zfp_field_1d(NULL, *zt, dims_used[0]); break;
            case 2: field = zfp_field_2d(NULL, *zt, dims_used[1], dims_used[0]); break;
            case 3: field = zfp_field_3d(NULL, *zt, dims_used[2], dims_used[1], dims_used[0]); break;
#if ZFP_VERSION_NO >= 0x0540
            case 4: field = zfp_field_4d(NULL, *zt, dims_used[3], dims_used[2], dims_used[1], dims_used[0]); break;
#endif
        }
        if (!field) PUSH_AND_GOTO(H5E_NOSPACE, -1, "zfp_field alloc failed");
        uint64_t meta = zfp_field_metadata(field);
        zfp_field_free(field);
        if (meta == ZFP_META_NULL) {
            *why = "chunk dimensions are too large for the ZFP header";
            goto done;
        }
    }

    /* The filter sees the data in the file's byte order */
    if (0 > (native_type_id = H5Tget_native_type(type_id, H5T_DIR_ASCEND)))
        PUSH_AND_GOTO(H5E_BADTYPE, -1, "unable to get native datatype");
    if (H5Tget_order(type_id) != H5Tget_order(native_type_id)) {
        *why = "requires a datatype in native byte order";
        goto done;
    }

    retval = 1;
done:
    if (native_type_id >= 0) H5Tclose(native_type_id);
    return retval;
}

static htri_t H5Z_zfp_can_apply(hid_t dcpl_id, hid_t type_id, hid_t chunk_space_id) {
    int ndims_used;
    hsize_t dims_used[H5S_MAX_RANK];
    zfp_type zt;
    const char *why;
    htri_t retval = 0;

    if ((int) stream_word_bits != 8)
        PUSH_AND_GOTO(H5E_CANTINIT, -1, "ZFP lib not compiled with -DBIT_STREAM_WORD_TYPE=uint8");

    /* A mandatory filter that cannot apply fails H5Dcreate2(), with this reason */
    if (0 == (retval = zfp_chunk_supported(type_id, chunk_space_id, &zt, &ndims_used, dims_used, &why)))
        H5Epush(H5E_DEFAULT, __FILE__, __func__, __LINE__, H5E_ERR_CLS, H5E_PLINE, H5E_CANAPPLY, "%s", why);

done:
    return retval;
}

static int get_zfp_info_from_cd_values(size_t cd_nelmts, unsigned int const *cd_values, uint64_t *zfp_mode, uint64_t *zfp_meta, H5T_order_t *swap);

static herr_t H5Z_zfp_set_local(hid_t dcpl_id, hid_t type_id, hid_t chunk_space_id) {
    int ndims_used = 0;
    size_t hdr_bits, hdr_bytes;
    size_t mem_cd_nelmts = H5Z_ZFP_CD_NELMTS_MEM;
    unsigned int mem_cd_values[H5Z_ZFP_CD_NELMTS_MEM] = {0};
    size_t hdr_cd_nelmts = H5Z_ZFP_CD_NELMTS_MAX;
    unsigned int hdr_cd_values[H5Z_ZFP_CD_NELMTS_MAX] = {0};
    unsigned int flags = 0;
    herr_t retval = 0;
    htri_t supported;
    hsize_t dims_used[H5S_MAX_RANK];
    zfp_type zt;
    const char *why;
    zfp_field *dummy_field = NULL;
    bitstream *dummy_bstr = NULL;
    zfp_stream *dummy_zstr = NULL;
    int have_mode_list, have_zfp_controls = 0, have_header = 0;
    h5z_zfp_controls_t ctrls;

    if (0 > H5Pget_filter_by_id(dcpl_id, H5Z_FILTER_ZFP, &flags, &mem_cd_nelmts, mem_cd_values, 0, NULL, NULL))
        PUSH_AND_GOTO(H5E_CANTGET, -1, "unable to get current ZFP cd_values");

    hdr_cd_values[0] = (unsigned int) ((ZFP_VERSION_NO<<16) | (ZFP_CODEC<<12) | H5Z_FILTER_ZFP_VERSION_NO);

    if (0 > (supported = zfp_chunk_supported(type_id, chunk_space_id, &zt, &ndims_used, dims_used, &why)))
        PUSH_AND_GOTO(H5E_BADTYPE, -1, "unable to check datatype and chunk shape");

    /* A list copied from a dataset ZFP declined holds no mode to apply */
    if (supported && mem_cd_nelmts == H5Z_ZFP_CD_NELMTS_DECLINE && mem_cd_values[1] == 0 &&
        0 >= H5Pexist(dcpl_id, "zfp_controls")) {
        supported = 0;
        why = "the ZFP parameters were copied from a dataset ZFP declined";
    }

    if (!supported) {
        /* can_apply() turned this chunk down, which fails H5Dcreate2() for a
           mandatory filter, so this one is optional.  It cannot take itself
           out of the pipeline here: HDF5 is walking the pipeline as it calls
           us, and removing an entry shifts the others under it.  Record values
           that make the filter refuse every chunk instead, so HDF5 stores them
           all unfiltered.  A copy of such a list is declined again. */
        if (!(flags & H5Z_FLAG_OPTIONAL)) {
            H5Epush(H5E_DEFAULT, __FILE__, __func__, __LINE__, H5E_ERR_CLS, H5E_PLINE, H5E_CANAPPLY, "%s", why);
            retval = -1;
            goto done;
        }
        if (0 > H5Pmodify_filter(dcpl_id, H5Z_FILTER_ZFP, flags, H5Z_ZFP_CD_NELMTS_DECLINE, hdr_cd_values))
            PUSH_AND_GOTO(H5E_BADVALUE, -1, "failed to modify cd_values");
        retval = 1;
        goto done;
    }

    switch (ndims_used) {
        case 1: dummy_field = zfp_field_1d(0, zt, dims_used[0]); break;
        case 2: dummy_field = zfp_field_2d(0, zt, dims_used[1], dims_used[0]); break;
        case 3: dummy_field = zfp_field_3d(0, zt, dims_used[2], dims_used[1], dims_used[0]); break;
#if ZFP_VERSION_NO >= 0x0540
        case 4: dummy_field = zfp_field_4d(0, zt, dims_used[3], dims_used[2], dims_used[1], dims_used[0]); break;
#endif
        default: PUSH_AND_GOTO(H5E_BADVALUE, -1, "chunks may have only 1...3/4 non-unity dims");
    }
    if (!dummy_field) PUSH_AND_GOTO(H5E_NOSPACE, -1, "zfp_field alloc failed");

    /* The mode comes from cd_values that name one, else from the property the
       H5Pset_zfp_*() helpers set, else from a ZFP header in cd_values: one
       this function recorded for another dataset whose creation property list
       was copied (h5repack does that, for one).  No cd_values at all means
       ZFP's expert-mode defaults. */
    have_mode_list = mem_cd_nelmts > 0 &&
        mem_cd_values[0] >= H5Z_ZFP_MODE_RATE && mem_cd_values[0] <= H5Z_ZFP_MODE_REVERSIBLE;

    if (!have_mode_list) {
        if (0 < H5Pexist(dcpl_id, "zfp_controls")) {
            if (0 > H5Pget(dcpl_id, "zfp_controls", &ctrls))
                PUSH_AND_GOTO(H5E_CANTGET, -1, "unable to get ZFP controls");
            have_zfp_controls = 1;
        } else if (mem_cd_nelmts == 0) {
            mem_cd_nelmts = H5Z_ZFP_CD_NELMTS_MEM;
            /* ZFP default: Expert mode defaults */
            mem_cd_values[0] = H5Z_ZFP_MODE_EXPERT;
            mem_cd_values[2] = ZFP_MIN_BITS; mem_cd_values[3] = ZFP_MAX_BITS; 
            mem_cd_values[4] = ZFP_MAX_PREC; mem_cd_values[5] = ZFP_MIN_EXP;
        } else {
            have_header = 1;
        }
    }

    if (0 == (dummy_bstr = stream_open(&hdr_cd_values[1], sizeof(hdr_cd_values))))
        PUSH_AND_GOTO(H5E_NOSPACE, -1, "stream_open() failed");

    if (0 == (dummy_zstr = zfp_stream_open(dummy_bstr)))
        PUSH_AND_GOTO(H5E_NOSPACE, -1, "zfp_stream_open() failed");

    if (have_header) {
        /* That header describes the other dataset's chunks; keep only its mode */
        uint64_t zfp_mode, zfp_meta;
        H5T_order_t swap = H5T_ORDER_NONE;

        if (mem_cd_nelmts < 2 || mem_cd_nelmts > H5Z_ZFP_CD_NELMTS_MEM ||
            0 == get_zfp_info_from_cd_values(mem_cd_nelmts - 1, &mem_cd_values[1], &zfp_mode, &zfp_meta, &swap) ||
            zfp_mode_null == zfp_stream_set_mode(dummy_zstr, zfp_mode))
            PUSH_AND_GOTO(H5E_BADVALUE, -1, "cd_values hold neither a ZFP mode nor a ZFP header");

        /* A fixed rate is stored as bits per block, and a block of d dimensions holds 4^d values */
        if (zfp_stream_compression_mode(dummy_zstr) == zfp_mode_fixed_rate) {
            zfp_field *src_field = zfp_field_alloc();
            if (!src_field || !zfp_field_set_metadata(src_field, zfp_meta)) {
                if (src_field) zfp_field_free(src_field);
                PUSH_AND_GOTO(H5E_BADVALUE, -1, "unable to read the ZFP header in cd_values");
            }
            double rate = zfp_stream_rate(dummy_zstr, zfp_field_dimensionality(src_field));
            zfp_field_free(src_field);
            zfp_stream_set_rate(dummy_zstr, rate, zt, ndims_used, 0);
        }
    } else if (have_zfp_controls) {
        switch (ctrls.mode) {
            case H5Z_ZFP_MODE_RATE:
                zfp_stream_set_rate(dummy_zstr, ctrls.details.rate, zt, ndims_used, 0); break;
            case H5Z_ZFP_MODE_PRECISION:
#if ZFP_VERSION_NO < 0x0510
                zfp_stream_set_precision(dummy_zstr, ctrls.details.prec, zt); break;
#else
                zfp_stream_set_precision(dummy_zstr, ctrls.details.prec); break;
#endif
            case H5Z_ZFP_MODE_ACCURACY:
#if ZFP_VERSION_NO < 0x0510
                zfp_stream_set_accuracy(dummy_zstr, ctrls.details.acc, zt); break;
#else
                zfp_stream_set_accuracy(dummy_zstr, ctrls.details.acc); break;
#endif
            case H5Z_ZFP_MODE_EXPERT:
                zfp_stream_set_params(dummy_zstr, ctrls.details.expert.minbits, ctrls.details.expert.maxbits, ctrls.details.expert.maxprec, ctrls.details.expert.minexp); break;
#if ZFP_VERSION_NO >= 0x0550
            case H5Z_ZFP_MODE_REVERSIBLE:
                zfp_stream_set_reversible(dummy_zstr); break;
#endif
            default: PUSH_AND_GOTO(H5E_BADVALUE, -1, "invalid ZFP mode");
        }
    } else {
        /* Define a temporary variable for strict-aliasing safe type punning */
        double tmp_dbl = 0.0; 
        
        switch (mem_cd_values[0]) {
            case H5Z_ZFP_MODE_RATE:
                memcpy(&tmp_dbl, &mem_cd_values[2], sizeof(double));
                zfp_stream_set_rate(dummy_zstr, tmp_dbl, zt, ndims_used, 0); break;
            case H5Z_ZFP_MODE_PRECISION:
#if ZFP_VERSION_NO < 0x0510
                zfp_stream_set_precision(dummy_zstr, mem_cd_values[2], zt); break;
#else
                zfp_stream_set_precision(dummy_zstr, mem_cd_values[2]); break;
#endif
            case H5Z_ZFP_MODE_ACCURACY:
                memcpy(&tmp_dbl, &mem_cd_values[2], sizeof(double));
#if ZFP_VERSION_NO < 0x0510
                zfp_stream_set_accuracy(dummy_zstr, tmp_dbl, zt); break;
#else
                zfp_stream_set_accuracy(dummy_zstr, tmp_dbl); break;
#endif
            case H5Z_ZFP_MODE_EXPERT:
                zfp_stream_set_params(dummy_zstr, mem_cd_values[2], mem_cd_values[3], mem_cd_values[4], (int) mem_cd_values[5]); break;
#if ZFP_VERSION_NO >= 0x0550
            case H5Z_ZFP_MODE_REVERSIBLE:
                zfp_stream_set_reversible(dummy_zstr); break;
#endif
            default: PUSH_AND_GOTO(H5E_BADVALUE, -1, "invalid ZFP mode");
        }
    }

    if (0 == (hdr_bits = zfp_write_header(dummy_zstr, dummy_field, ZFP_HEADER_FULL)))
        PUSH_AND_GOTO(H5E_CANTINIT, -1, "unable to write header");

    zfp_stream_flush(dummy_zstr);

    hdr_bytes = 1 + ((hdr_bits  - 1) / 8);
    hdr_cd_nelmts = 1 + ((hdr_bytes - 1) / sizeof(hdr_cd_values[0]));
    hdr_cd_nelmts++; 

    if (hdr_cd_nelmts > H5Z_ZFP_CD_NELMTS_MAX) PUSH_AND_GOTO(H5E_BADVALUE, -1, "buffer overrun in hdr_cd_values");
    if (0 > H5Pmodify_filter(dcpl_id, H5Z_FILTER_ZFP, flags, hdr_cd_nelmts, hdr_cd_values)) PUSH_AND_GOTO(H5E_BADVALUE, -1, "failed to modify cd_values");

    retval = 1;
done:
    if (dummy_field) zfp_field_free(dummy_field);
    if (dummy_zstr) zfp_stream_close(dummy_zstr);
    if (dummy_bstr) stream_close(dummy_bstr);
    return retval;
}

static int get_zfp_info_from_cd_values(size_t cd_nelmts, unsigned int const *cd_values, uint64_t *zfp_mode, uint64_t *zfp_meta, H5T_order_t *swap) {
    unsigned int cd_values_copy[H5Z_ZFP_CD_NELMTS_MAX];
    int retval = 0;
    bitstream *bstr = NULL;
    zfp_stream *zstr = NULL;
    zfp_field *zfld = NULL;

    if (cd_nelmts > H5Z_ZFP_CD_NELMTS_MAX) PUSH_AND_GOTO(H5E_OVERFLOW, 0, "cd_nelmts exceeds max");

    memcpy(cd_values_copy, cd_values, cd_nelmts * sizeof(cd_values[0]));

    if (0 == (bstr = stream_open(&cd_values_copy[0], sizeof(cd_values_copy[0]) * cd_nelmts))) PUSH_AND_GOTO(H5E_NOSPACE, 0, "opening header bitstream failed");
    if (0 == (zstr = zfp_stream_open(bstr))) PUSH_AND_GOTO(H5E_NOSPACE, 0, "opening header zfp stream failed");
    if (0 == (zfld = zfp_field_alloc())) PUSH_AND_GOTO(H5E_NOSPACE, 0, "allocating field failed");

    if (0 == (zfp_read_header(zstr, zfld, ZFP_HEADER_MAGIC))) {
        herr_t conv;
        if (H5T_ORDER_LE == (*swap = (H5Tget_order(H5T_NATIVE_UINT))))
            conv = H5Tconvert(H5T_STD_U32BE, H5T_NATIVE_UINT, cd_nelmts, cd_values_copy, 0, H5P_DEFAULT);
        else
            conv = H5Tconvert(H5T_STD_U32LE, H5T_NATIVE_UINT, cd_nelmts, cd_values_copy, 0, H5P_DEFAULT);
        
        if (conv < 0) PUSH_AND_GOTO(H5E_BADVALUE, 0, "header endian-swap failed");

        zfp_stream_rewind(zstr);
        if (0 == (zfp_read_header(zstr, zfld, ZFP_HEADER_MAGIC))) PUSH_AND_GOTO(H5E_CANTGET, 0, "ZFP codec version mismatch");
    }
    zfp_stream_rewind(zstr);

    if (0 == (zfp_read_header(zstr, zfld, ZFP_HEADER_FULL))) PUSH_AND_GOTO(H5E_CANTGET, 0, "reading header failed");

    *zfp_mode = zfp_stream_mode(zstr);
    *zfp_meta = zfp_field_metadata(zfld);

    retval = 1;
done:
    if (zfld) zfp_field_free(zfld);
    if (zstr) zfp_stream_close(zstr);
    if (bstr) stream_close(bstr);
    return retval;
}

static int zfp_codec_version_mismatch(unsigned int h5zfpver_from_cd_val, unsigned int zfpver_from_cd_val, unsigned int zfpcodec_from_cd_val) {
    int writer_codec, reader_codec;
    if (h5zfpver_from_cd_val < 0x0110) {
        zfpver_from_cd_val <<= 4;
        if (zfpver_from_cd_val < 0x0500) writer_codec = 4;
        else if (zfpver_from_cd_val < 0x1000) writer_codec = (zfpver_from_cd_val & 0x0F00)>>8;
        else writer_codec = 5;
    } else {
        writer_codec = zfpcodec_from_cd_val;
    }

#if ZFP_VERSION_NO < 0x0500
    reader_codec = 4;
#elif ZFP_VERSION_NO < 0x1000
    reader_codec = 5;
#else
    reader_codec = ZFP_CODEC;
#endif

    return writer_codec > reader_codec;
}

/* ZFP's lossy modes have no encoding for NaN or Inf: one turns into an
   arbitrary finite number and spoils the other values in its block, too.
   Refuse such a chunk instead.  With H5Z_FLAG_OPTIONAL, HDF5 then stores that
   one chunk unfiltered, so the data stays exact; with H5Z_FLAG_MANDATORY the
   write fails rather than corrupting it. */
static int has_nonfinite(const void *buf, size_t nbytes, zfp_type type) {
    const unsigned char *p = (const unsigned char *)buf;
    if (type == zfp_type_double) {
        for (size_t i = 0; i + 8 <= nbytes; i += 8) {
            uint64_t u;
            memcpy(&u, p + i, 8);
            if ((u & UINT64_C(0x7FF0000000000000)) == UINT64_C(0x7FF0000000000000)) return 1;
        }
    } else if (type == zfp_type_float) {
        for (size_t i = 0; i + 4 <= nbytes; i += 4) {
            uint32_t u;
            memcpy(&u, p + i, 4);
            if ((u & UINT32_C(0x7F800000)) == UINT32_C(0x7F800000)) return 1;
        }
    }
    return 0;
}

static size_t H5Z_filter_zfp(unsigned int flags, size_t cd_nelmts, const unsigned int cd_values[], size_t nbytes, size_t *buf_size, void **buf) {
    if (nbytes == 0) return 0;
    
    void *newbuf = NULL;
    void *scratch = NULL;  /* padded copy to decode, or byte-swapped copy to encode */
    size_t retval = 0;
    unsigned int cd_vals_h5zzfpver, cd_vals_zfpcodec, cd_vals_zfpver;
    H5T_order_t swap = H5T_ORDER_NONE;
    uint64_t zfp_mode, zfp_meta;
    bitstream *bstr = NULL;
    zfp_stream *zstr = NULL;
    zfp_field *zfld = NULL;

    /* The version word and a ZFP header take more than this.  Fewer values are
       what set_local() records for a chunk ZFP cannot take, or, with none at
       all (when HDF5 passes cd_values = NULL), what earlier versions of this
       filter left for one. */
    if (cd_nelmts <= H5Z_ZFP_CD_NELMTS_DECLINE)
        PUSH_AND_GOTO(H5E_CANTFILTER, 0, "no ZFP header in the filter parameters: ZFP cannot compress chunks of this datatype or shape");

    cd_vals_h5zzfpver = cd_values[0]&0x00000FFF;
    cd_vals_zfpcodec = (cd_values[0]>>12)&0x0000000F;
    cd_vals_zfpver = (cd_values[0]>>16)&0x0000FFFF;

    if (0 == get_zfp_info_from_cd_values(cd_nelmts-1, &cd_values[1], &zfp_mode, &zfp_meta, &swap)) PUSH_AND_GOTO(H5E_CANTGET, 0, "can't get ZFP mode/meta");

    if (flags & H5Z_FLAG_REVERSE) {
        size_t status, msize;
        size_t bsize, dsize;
        void *src = *buf;
        size_t src_size = nbytes;

        if (zfp_codec_version_mismatch(cd_vals_h5zzfpver, cd_vals_zfpver, cd_vals_zfpcodec)) PUSH_AND_GOTO(H5E_READERROR, 0, "ZFP codec version mismatch");

        if (0 == (zfld = zfp_field_alloc())) PUSH_AND_GOTO(H5E_NOSPACE, 0, "field alloc failed");
        zfp_field_set_metadata(zfld, zfp_meta);

        bsize = zfp_field_size(zfld, 0);
        switch (zfp_field_type(zfld)) {
            case zfp_type_int32:  dsize = sizeof(int32_t);  break;
            case zfp_type_int64:  dsize = sizeof(int64_t);  break;
            case zfp_type_float:  dsize = sizeof(float);  break;
            case zfp_type_double: dsize = sizeof(double); break;
            default: PUSH_AND_GOTO(H5E_BADTYPE, 0, "invalid datatype");
        }
        bsize *= dsize;

        if (NULL == (newbuf = H5allocate_memory(bsize, 0))) PUSH_AND_GOTO(H5E_NOSPACE, 0, "memory allocation failed for ZFP decompression");

        zfp_field_set_pointer(zfld, newbuf);

        if (0 == (zstr = zfp_stream_open(NULL))) PUSH_AND_GOTO(H5E_NOSPACE, 0, "zfp stream open failed");
        zfp_stream_set_mode(zstr, zfp_mode);

        /* zfp's bit stream does no bounds checking, so a truncated or corrupt
           chunk would be read past its end.  Decode a short one from a copy
           padded with zeros to as much as ZFP can ever read, and refuse it if
           decoding used more than the chunk holds. */
        msize = zfp_stream_maximum_size(zstr, zfld);
        if (nbytes < msize) {
            if (NULL == (scratch = H5allocate_memory(msize, 1))) PUSH_AND_GOTO(H5E_NOSPACE, 0, "memory allocation failed for ZFP decompression");
            memcpy(scratch, *buf, nbytes);
            src = scratch;
            src_size = msize;
        }

        if (0 == (bstr = stream_open(src, src_size))) PUSH_AND_GOTO(H5E_NOSPACE, 0, "bitstream open failed");
        zfp_stream_set_bit_stream(zstr, bstr);
        status = zfp_decompress(zstr, zfld);

        zfp_field_free(zfld); zfld = NULL;
        zfp_stream_close(zstr); zstr = NULL;
        stream_close(bstr); bstr = NULL;

        if (!status) PUSH_AND_GOTO(H5E_CANTFILTER, 0, "decompression failed");
        if (status > nbytes) PUSH_AND_GOTO(H5E_CANTFILTER, 0, "ZFP chunk is truncated or corrupt");

        if (swap != H5T_ORDER_NONE) {
            hid_t src = dsize == 4 ? H5T_STD_U32BE : H5T_STD_U64BE; 
            hid_t dst = dsize == 4 ? H5T_NATIVE_UINT32 : H5T_NATIVE_UINT64;
            if (swap == H5T_ORDER_BE) src = dsize == 4 ? H5T_STD_U32LE : H5T_STD_U64LE; 
            if (H5Tconvert(src, dst, bsize/dsize, newbuf, 0, H5P_DEFAULT) < 0) PUSH_AND_GOTO(H5E_BADVALUE, 0, "endian-UN-swap failed");
        }

        H5free_memory(*buf);
        *buf = newbuf;
        newbuf = NULL;
        *buf_size = bsize; 
        retval = bsize;
    } else {
        size_t msize, zsize, dsize;
        void *src = *buf;

        if (0 == (zfld = zfp_field_alloc())) PUSH_AND_GOTO(H5E_NOSPACE, 0, "field alloc failed");
        zfp_field_set_metadata(zfld, zfp_meta);

        /* The header must describe this very chunk.  One left from another
           dataset (a copied creation property list whose header another
           filter's set_local did not replace) would have ZFP read past the end
           of the buffer, or compress only part of it. */
        dsize = zfp_type_size(zfp_field_type(zfld));
        if (dsize == 0 || zfp_field_size(zfld, NULL) * dsize != nbytes)
            PUSH_AND_GOTO(H5E_CANTFILTER, 0, "ZFP header does not describe this chunk's size");

        /* A header written on a machine of the other byte order means the data
           is in that order too: byte-swap a copy to native before encoding,
           as the read path swaps the decoded values back */
        if (swap != H5T_ORDER_NONE) {
            hid_t from = dsize == 4 ? H5T_STD_U32BE : H5T_STD_U64BE;
            hid_t to   = dsize == 4 ? H5T_NATIVE_UINT32 : H5T_NATIVE_UINT64;
            if (swap == H5T_ORDER_BE) from = dsize == 4 ? H5T_STD_U32LE : H5T_STD_U64LE;
            if (NULL == (scratch = H5allocate_memory(nbytes, 0))) PUSH_AND_GOTO(H5E_NOSPACE, 0, "memory allocation failed for ZFP compression");
            memcpy(scratch, *buf, nbytes);
            if (H5Tconvert(from, to, nbytes / dsize, scratch, 0, H5P_DEFAULT) < 0) PUSH_AND_GOTO(H5E_BADVALUE, 0, "endian-swap failed");
            src = scratch;
        }
        zfp_field_set_pointer(zfld, src);

        if (0 == (zstr = zfp_stream_open(0))) PUSH_AND_GOTO(H5E_NOSPACE, 0, "zfp stream open failed");
        zfp_stream_set_mode(zstr, zfp_mode);

        /* Reversible mode reproduces any bit pattern, NaN and Inf included */
        if (zfp_stream_compression_mode(zstr) != zfp_mode_reversible &&
            has_nonfinite(src, nbytes, zfp_field_type(zfld)))
            PUSH_AND_GOTO(H5E_CANTFILTER, 0, "ZFP cannot encode the NaN or Inf values in this chunk");

        msize = zfp_stream_maximum_size(zstr, zfld);

        if (NULL == (newbuf = H5allocate_memory(msize, 0))) PUSH_AND_GOTO(H5E_NOSPACE, 0, "memory allocation failed for ZFP compression");
        if (0 == (bstr = stream_open(newbuf, msize))) PUSH_AND_GOTO(H5E_NOSPACE, 0, "bitstream open failed");

        zfp_stream_set_bit_stream(zstr, bstr);
        zsize = zfp_compress(zstr, zfld);

        zfp_field_free(zfld); zfld = NULL;
        zfp_stream_close(zstr); zstr = NULL;
        stream_close(bstr); bstr = NULL;

        if (zsize == 0) PUSH_AND_GOTO(H5E_CANTFILTER, 0, "compression failed");
        
        H5free_memory(*buf);
        *buf = newbuf;
        newbuf = NULL;
        *buf_size = zsize;
        retval = zsize;
    }

done:
    if (zfld) zfp_field_free(zfld);
    if (zstr) zfp_stream_close(zstr);
    if (bstr) stream_close(bstr);
    if (newbuf) H5free_memory(newbuf);
    if (scratch) H5free_memory(scratch);
    return retval ;
}

const H5Z_class2_t zfp_class = {
    H5Z_CLASS_T_VERS,
    H5Z_FILTER_ZFP,
    1, 1,
    "zfp",
    H5Z_zfp_can_apply,
    H5Z_zfp_set_local,
    H5Z_filter_zfp,
};
