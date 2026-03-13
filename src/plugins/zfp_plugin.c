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
    herr_t retval = 0;

    if (0 >= H5Pisa_class(plist, H5P_DATASET_CREATE))
        PUSH_AND_GOTO(H5E_BADTYPE, -1, "not a dataset creation property list class");

    ctrls_p = (h5z_zfp_controls_t *) H5allocate_memory(ctrls_sz, 0);
    if (!ctrls_p) PUSH_AND_GOTO(H5E_NOSPACE, -1, "allocation failed for ZFP controls");

    va_start(ap, mode);
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

static htri_t H5Z_zfp_can_apply(hid_t dcpl_id, hid_t type_id, hid_t chunk_space_id) {   
    int const max_ndims = (ZFP_VERSION_NO <= 0x0053) ? 3 : 4;
    int ndims, ndims_used = 0;
    size_t dsize;
    htri_t retval = 0;
    hsize_t dims[H5S_MAX_RANK];
    H5T_class_t dclass;
    hid_t native_type_id;

    if ((int) stream_word_bits != 8)
        PUSH_AND_GOTO(H5E_CANTINIT, -1, "ZFP lib not compiled with -DBIT_STREAM_WORD_TYPE=uint8");

    if (H5T_NO_CLASS == (dclass = H5Tget_class(type_id))) PUSH_AND_GOTO(H5E_BADTYPE, -1, "bad datatype class");
    if (0 == (dsize = H5Tget_size(type_id))) PUSH_AND_GOTO(H5E_BADTYPE, -1, "bad datatype size");
    if (0 > (ndims = H5Sget_simple_extent_dims(chunk_space_id, dims, 0))) PUSH_AND_GOTO(H5E_BADTYPE, -1, "bad chunk data space");

#if ZFP_VERSION_NO < 0x0510
    if (!(dclass == H5T_FLOAT)) PUSH_AND_GOTO(H5E_BADTYPE, 0, "requires datatype class of H5T_FLOAT");
#else
    if (!(dclass == H5T_FLOAT || dclass == H5T_INTEGER)) PUSH_AND_GOTO(H5E_BADTYPE, 0, "requires datatype class of H5T_FLOAT or H5T_INTEGER");
#endif

    if (!(dsize == 4 || dsize == 8)) PUSH_AND_GOTO(H5E_BADTYPE, 0, "requires datatype size of 4 or 8");

    for (int i = 0; i < ndims; i++) {
        if (dims[i] <= 1) continue;
        ndims_used++;
    }

    if (ndims_used == 0 || ndims_used > max_ndims)
#if ZFP_VERSION_NO < 0x0530
        PUSH_AND_GOTO(H5E_BADVALUE, 0, "chunk must have only 1...3 non-unity dimensions");
#else
        PUSH_AND_GOTO(H5E_BADVALUE, 0, "chunk must have only 1...4 non-unity dimensions");
#endif

    native_type_id = H5Tget_native_type(type_id, H5T_DIR_ASCEND);
    if (H5Tget_order(type_id) != H5Tget_order(native_type_id))
        PUSH_AND_GOTO(H5E_BADTYPE, 0, "endian targetting non-sensical in conjunction with ZFP filter");

    retval = 1;
done:
    return retval;
}

static herr_t H5Z_zfp_set_local(hid_t dcpl_id, hid_t type_id, hid_t chunk_space_id) {   
    int ndims, ndims_used = 0;
    size_t dsize, hdr_bits, hdr_bytes;
    size_t mem_cd_nelmts = H5Z_ZFP_CD_NELMTS_MEM;
    unsigned int mem_cd_values[H5Z_ZFP_CD_NELMTS_MEM] = {0};
    size_t hdr_cd_nelmts = H5Z_ZFP_CD_NELMTS_MAX;
    unsigned int hdr_cd_values[H5Z_ZFP_CD_NELMTS_MAX] = {0};
    unsigned int flags = 0;
    herr_t retval = 0;
    hsize_t dims[H5S_MAX_RANK], dims_used[H5S_MAX_RANK];
    H5T_class_t dclass;
    zfp_type zt;
    zfp_field *dummy_field = NULL;
    bitstream *dummy_bstr = NULL;
    zfp_stream *dummy_zstr = NULL;
    int have_zfp_controls = 0;
    h5z_zfp_controls_t ctrls;

    if (0 > (dclass = H5Tget_class(type_id))) PUSH_AND_GOTO(H5E_BADTYPE, -1, "not a datatype");
    if (0 == (dsize = H5Tget_size(type_id))) PUSH_AND_GOTO(H5E_BADTYPE, -1, "not a datatype");
    if (0 > (ndims = H5Sget_simple_extent_dims(chunk_space_id, dims, 0))) PUSH_AND_GOTO(H5E_BADTYPE, -1, "not a data space");

    if (dclass == H5T_FLOAT) {
        if (dsize == sizeof(float)) zt = zfp_type_float;
        else if (dsize == sizeof(double)) zt = zfp_type_double;
        else PUSH_AND_GOTO(H5E_BADTYPE, -1, "invalid datatype size");
    } else if (dclass == H5T_INTEGER) {
        if (dsize == sizeof(int32_t)) zt = zfp_type_int32;
        else if (dsize == sizeof(int64_t)) zt = zfp_type_int64;
        else PUSH_AND_GOTO(H5E_BADTYPE, -1, "invalid datatype size");
    } else {
        PUSH_AND_GOTO(H5E_BADTYPE, 0, "datatype class must be H5T_FLOAT or H5T_INTEGER");
    }

    for (int i = 0; i < ndims; i++) {
        if (dims[i] <= 1) continue;
        dims_used[ndims_used++] = dims[i];
    }

    switch (ndims_used) {
        case 1: dummy_field = zfp_field_1d(0, zt, dims_used[0]); break;
        case 2: dummy_field = zfp_field_2d(0, zt, dims_used[1], dims_used[0]); break;
        case 3: dummy_field = zfp_field_3d(0, zt, dims_used[2], dims_used[1], dims_used[0]); break;
#if ZFP_VERSION_NO >= 0x0540
        case 4: dummy_field = zfp_field_4d(0, zt, dims_used[3], dims_used[2], dims_used[1], dims_used[0]); break;
#endif
        default: PUSH_AND_GOTO(H5E_BADVALUE, 0, "chunks may have only 1...3/4 non-unity dims");
    }
    if (!dummy_field) PUSH_AND_GOTO(H5E_NOSPACE, 0, "zfp_field alloc failed");

    if (0 > H5Pget_filter_by_id(dcpl_id, H5Z_FILTER_ZFP, &flags, &mem_cd_nelmts, mem_cd_values, 0, NULL, NULL))
        PUSH_AND_GOTO(H5E_CANTGET, 0, "unable to get current ZFP cd_values");

    if (mem_cd_nelmts == 0) {
        if (0 < H5Pexist(dcpl_id, "zfp_controls")) {
            if (0 > H5Pget(dcpl_id, "zfp_controls", &ctrls))
                PUSH_AND_GOTO(H5E_CANTGET, 0, "unable to get ZFP controls");
            have_zfp_controls = 1;
        } else {
            mem_cd_nelmts = H5Z_ZFP_CD_NELMTS_MEM;
            /* ZFP default: Expert mode defaults */
            mem_cd_values[0] = H5Z_ZFP_MODE_EXPERT;
            mem_cd_values[2] = ZFP_MIN_BITS; mem_cd_values[3] = ZFP_MAX_BITS; 
            mem_cd_values[4] = ZFP_MAX_PREC; mem_cd_values[5] = ZFP_MIN_EXP;
        }
    }
        
    hdr_cd_values[0] = (unsigned int) ((ZFP_VERSION_NO<<16) | (ZFP_CODEC<<12) | H5Z_FILTER_ZFP_VERSION_NO);
    if (0 == (dummy_bstr = stream_open(&hdr_cd_values[1], sizeof(hdr_cd_values))))
        PUSH_AND_GOTO(H5E_NOSPACE, 0, "stream_open() failed");

    if (0 == (dummy_zstr = zfp_stream_open(dummy_bstr)))
        PUSH_AND_GOTO(H5E_NOSPACE, 0, "zfp_stream_open() failed");

    if (have_zfp_controls) {
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
            default: PUSH_AND_GOTO(H5E_BADVALUE, 0, "invalid ZFP mode");
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
            default: PUSH_AND_GOTO(H5E_BADVALUE, 0, "invalid ZFP mode");
        }
    }

    if (0 == (hdr_bits = zfp_write_header(dummy_zstr, dummy_field, ZFP_HEADER_FULL)))
        PUSH_AND_GOTO(H5E_CANTINIT, 0, "unable to write header");

    zfp_stream_flush(dummy_zstr);

    hdr_bytes = 1 + ((hdr_bits  - 1) / 8);
    hdr_cd_nelmts = 1 + ((hdr_bytes - 1) / sizeof(hdr_cd_values[0]));
    hdr_cd_nelmts++; 

    if (hdr_cd_nelmts > H5Z_ZFP_CD_NELMTS_MAX) PUSH_AND_GOTO(H5E_BADVALUE, -1, "buffer overrun in hdr_cd_values");
    if (0 > H5Pmodify_filter(dcpl_id, H5Z_FILTER_ZFP, flags, hdr_cd_nelmts, hdr_cd_values)) PUSH_AND_GOTO(H5E_BADVALUE, 0, "failed to modify cd_values");

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

static size_t H5Z_filter_zfp(unsigned int flags, size_t cd_nelmts, const unsigned int cd_values[], size_t nbytes, size_t *buf_size, void **buf) {
    if (nbytes == 0) return 0;
    
    void *newbuf = NULL;
    size_t retval = 0;
    unsigned int cd_vals_h5zzfpver = cd_values[0]&0x00000FFF;
    unsigned int cd_vals_zfpcodec = (cd_values[0]>>12)&0x0000000F;
    unsigned int cd_vals_zfpver = (cd_values[0]>>16)&0x0000FFFF;
    H5T_order_t swap = H5T_ORDER_NONE;
    uint64_t zfp_mode, zfp_meta;
    bitstream *bstr = NULL;
    zfp_stream *zstr = NULL;
    zfp_field *zfld = NULL;

    if (0 == get_zfp_info_from_cd_values(cd_nelmts-1, &cd_values[1], &zfp_mode, &zfp_meta, &swap)) PUSH_AND_GOTO(H5E_CANTGET, 0, "can't get ZFP mode/meta");

    if (flags & H5Z_FLAG_REVERSE) {
        int status;
        size_t bsize, dsize;

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

        if (0 == (bstr = stream_open(*buf, *buf_size))) PUSH_AND_GOTO(H5E_NOSPACE, 0, "bitstream open failed");
        if (0 == (zstr = zfp_stream_open(bstr))) PUSH_AND_GOTO(H5E_NOSPACE, 0, "zfp stream open failed");

        zfp_stream_set_mode(zstr, zfp_mode);
        status = zfp_decompress(zstr, zfld);

        zfp_field_free(zfld); zfld = NULL;
        zfp_stream_close(zstr); zstr = NULL;
        stream_close(bstr); bstr = NULL;

        if (!status) PUSH_AND_GOTO(H5E_CANTFILTER, 0, "decompression failed");

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
        size_t msize, zsize;

        if (0 == (zfld = zfp_field_alloc())) PUSH_AND_GOTO(H5E_NOSPACE, 0, "field alloc failed");
        zfp_field_set_pointer(zfld, *buf);
        zfp_field_set_metadata(zfld, zfp_meta);

        if (0 == (zstr = zfp_stream_open(0))) PUSH_AND_GOTO(H5E_NOSPACE, 0, "zfp stream open failed");
        zfp_stream_set_mode(zstr, zfp_mode);
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
