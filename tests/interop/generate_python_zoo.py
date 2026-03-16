import os
import sys
import h5py
import hdf5plugin
import numpy as np

print("=" * 60)
print("=== STARTING ENCODING ZOO GENERATOR (FINAL) ===")
print("=" * 60)
sys.stdout.flush()

FILENAME = "encoding_zoo.h5"

def generate_compressible_data(shape, dtype):
    """
    Generates deterministic data tailored to prevent C-library math panics.
    """
    total_elements = np.prod(shape)
    
    if np.issubdtype(dtype, np.floating):
        # Many advanced float compressors (like SZ) use predictors or wavelets. 
        # A perfectly linear sequence has zero variance and can cause divide-by-zero 
        # errors in poorly written C-extensions. 
        # We combine a linear base with a gentle sine wave to guarantee mathematical entropy.
        base = np.linspace(0.0, 1.0, total_elements, dtype=dtype)
        wave = (np.sin(np.linspace(0, 50, total_elements)) * 0.1).astype(dtype)
        return (base + wave).reshape(shape)
    else:
        # For integers, a simple sequential count is highly compressible by LZ/dictionary 
        # methods and makes hex-dump debugging much easier.
        return np.arange(1, total_elements + 1, dtype=dtype).reshape(shape)

def main():
    # Start fresh every run
    if os.path.exists(FILENAME):
        os.remove(FILENAME)

    # Shape is set to (128, 128) because certain filters (like szip and bitshuffle) 
    # strictly require chunk dimensions to be divisible by 8 or 16.
    shape = (128, 128) 
    
    # Configuration Format: 
    # (Dataset Name, Numpy Data Type, hdf5plugin Config, Strict cd_values Check, Is Lossless)
    plugin_configs = [
        # --- Direct Plugins (Lossless) ---
        ("bzip2", np.int64, hdf5plugin.BZip2(blocksize=9), True, True),
        ("lz4", np.float32, hdf5plugin.LZ4(nbytes=0), True, True),
        ("zstd", np.int16, hdf5plugin.Zstd(clevel=5), True, True),
        
        # FciDecomp (JPEG-LS) strictly requires 1-byte or 2-byte integers. 
        # uint8 is the safest choice to avoid C-level panics.
        ("fcidecomp", np.uint8, hdf5plugin.FciDecomp(), True, True),
        
        # --- Bitshuffle (Lossless) ---
        ("bitshuffle_none", np.int32, hdf5plugin.Bitshuffle(cname='none'), True, True),
        ("bitshuffle_lz4", np.int32, hdf5plugin.Bitshuffle(cname='lz4'), True, True),
        ("bitshuffle_zstd", np.int64, hdf5plugin.Bitshuffle(cname='zstd', clevel=5), True, True),
        
        # --- ZFP (Mixed Lossy/Lossless) ---
        # ZFP expects floating point data for most of its rate/precision modes.
        ("zfp_rate", np.float32, hdf5plugin.Zfp(rate=16.0), False, False),
        ("zfp_precision", np.float64, hdf5plugin.Zfp(precision=12), False, False),
        ("zfp_accuracy", np.float32, hdf5plugin.Zfp(accuracy=0.001), False, False),
        ("zfp_reversible", np.int32, hdf5plugin.Zfp(reversible=True), False, True), # Reversible is lossless
        ("zfp_expert", np.float64, hdf5plugin.Zfp(minbits=1, maxbits=16657, maxprec=64, minexp=-1074), False, False),
        
        # --- SZ / SZ3 (Lossy Float Compressors) ---
        ("sz_absolute", np.float32, hdf5plugin.SZ(absolute=0.1), False, False),
        ("sz_relative", np.float64, hdf5plugin.SZ(relative=0.01), False, False),
        ("sz3_absolute", np.float64, hdf5plugin.SZ3(absolute=0.1), False, False),
    ]

    # --- Exhaustive Blosc 1 (All Lossless) ---
    # We iterate through all major sub-codecs and apply no-shuffle, byte-shuffle, and bit-shuffle.
    for codec in ['blosclz', 'lz4', 'lz4hc', 'zlib', 'zstd', 'snappy']:
        plugin_configs.extend([
            (f"blosc_{codec}_noshuffle", np.int16, hdf5plugin.Blosc(cname=codec, shuffle=hdf5plugin.Blosc.NOSHUFFLE), True, True),
            (f"blosc_{codec}_shuffle", np.int32, hdf5plugin.Blosc(cname=codec, shuffle=hdf5plugin.Blosc.SHUFFLE), True, True),
            (f"blosc_{codec}_bitshuffle", np.float64, hdf5plugin.Blosc(cname=codec, shuffle=hdf5plugin.Blosc.BITSHUFFLE), True, True),
        ])

    # --- Exhaustive Blosc 2 (Mixed Lossy/Lossless) ---
    for codec in ['blosclz', 'lz4', 'lz4hc', 'zlib', 'zstd']:
        plugin_configs.extend([
            (f"blosc2_{codec}_nofilter", np.int16, hdf5plugin.Blosc2(cname=codec, filters=hdf5plugin.Blosc2.NOFILTER), True, True),
            (f"blosc2_{codec}_shuffle", np.int32, hdf5plugin.Blosc2(cname=codec, filters=hdf5plugin.Blosc2.SHUFFLE), True, True),
            (f"blosc2_{codec}_bitshuffle", np.float64, hdf5plugin.Blosc2(cname=codec, filters=hdf5plugin.Blosc2.BITSHUFFLE), True, True),
            (f"blosc2_{codec}_delta", np.int32, hdf5plugin.Blosc2(cname=codec, filters=hdf5plugin.Blosc2.DELTA), True, True),
            
            # TRUNC_PREC zeroes out least significant bits; it is strictly lossy and requires floats!
            (f"blosc2_{codec}_truncprec", np.float32, hdf5plugin.Blosc2(cname=codec, filters=hdf5plugin.Blosc2.TRUNC_PREC), True, False),
        ])

    # Finalize the list by appending native h5py built-ins
    configs = [
        # Native built-ins bypass hdf5plugin and rely on h5py's internal C bindings.
        # Fixed the gzip filter ID from 8 to 1.
        ("gzip_deflate", np.int32, {"compression": "gzip", "compression_opts": 5}, 1, False, True),
        ("szip_ec", np.int32, {"compression": "szip", "compression_opts": ("ec", 8)}, 4, False, True),
        ("szip_nn", np.int32, {"compression": "szip", "compression_opts": ("nn", 8)}, 4, False, True),
        ("lzf", np.int32, {"compression": "lzf"}, 32000, False, True),
    ]

    # Unpack the dynamically generated filter IDs from the hdf5plugin kwargs
    for name, dtype, kwargs, strict_check, is_lossless in plugin_configs:
        fid = kwargs['compression'] 
        configs.append((name, dtype, kwargs, fid, strict_check, is_lossless))

    # ==========================================
    # PHASE 1: Generate the File
    # ==========================================
    print(f"\n--- Phase 1: Generating {FILENAME} ---")
    sys.stdout.flush()
    
    try:
        with h5py.File(FILENAME, 'w') as f:
            for name, dtype, kwargs, expected_fid, strict_check, is_lossless in configs:
                data = generate_compressible_data(shape, dtype)
                
                print(f"--> [START] Creating {name: <30} | Type: {dtype.__name__: <7} ...", end=" ")
                sys.stdout.flush()
                
                try:
                    f.create_dataset(name, data=data, chunks=shape, **kwargs)
                    print(f"DONE (ID: {expected_fid})")
                except Exception as e:
                    print(f"FAILED (Caught Exception: {e})")
                sys.stdout.flush()
                
        print("\n--> [SUCCESS] HDF5 file closed gracefully!")
        sys.stdout.flush()

    except Exception as e:
        print(f"\n!!! FATAL ERROR DURING FILE CLOSURE: {e} !!!")
        sys.stdout.flush()
        return

    # ==========================================
    # PHASE 2: Verify Metadata & Read-back
    # ==========================================
    print(f"\n--- Phase 2: Verifying {FILENAME} ---")
    sys.stdout.flush()
    
    passed_tests = 0
    total_written = 0
    
    with h5py.File(FILENAME, 'r') as f:
        for name, dtype, kwargs, expected_fid, strict_check, is_lossless in configs:
            if name not in f:
                continue
            
            total_written += 1
            dset = f[name]
            print(f"[{name}] -> Reading metadata/data...", end=" ")
            sys.stdout.flush()
            
            # Step A: Validate Filter ID Metadata
            plist = dset.id.get_create_plist()
            nfilters = plist.get_nfilters()
            
            found_fid = False
            for i in range(nfilters):
                fid, flags, cd_values, fname = plist.get_filter(i)
                if fid == expected_fid:
                    found_fid = True
                    break
            
            if not found_fid:
                print(f" FAILED (Missing ID {expected_fid})")
                continue

            # Step B: Validate Data Decompression
            original_data = generate_compressible_data(shape, dtype)
            try:
                # Trigger the C-level decompression pipeline
                read_data = dset[...] 
                
                if is_lossless:
                    # Lossless filters must produce an exact bit-for-bit match
                    if np.array_equal(read_data, original_data):
                        print("OK")
                        passed_tests += 1
                    else:
                        print("FAILED (Data Mismatch)")
                else:
                    # Lossy filters will modify the data, so we only assert that 
                    # decompression didn't crash and the shape/type remain intact.
                    if read_data.shape == original_data.shape:
                        print("OK")
                        passed_tests += 1
                    else:
                        print("FAILED (Shape Mismatch)")
                        
            except Exception as e:
                print(f"ERROR ({e})")
            
            sys.stdout.flush()

    print(f"\n=== ZOO COMPLETE: {passed_tests}/{total_written} verified ===")

if __name__ == "__main__":
    main()