import h5py
import numpy as np
import hdf5plugin

# Generate both float and integer test arrays
data_float = np.random.rand(1024).astype(np.float64)
data_int = np.arange(1024, dtype=np.int32)

# Use explicit hdf5plugin wrappers to prevent Blosc global state bleeding
configs = {
    "zlibng_gzip": {"compression": "gzip", "compression_opts": 9},
    "szip_ec": {"compression": "szip", "compression_opts": ("ec", 8)},
    "szip_nn": {"compression": "szip", "compression_opts": ("nn", 8)},
    "bzip2": hdf5plugin.BZip2(clevel=9),  # <-- Fixed capitalization here
    "lzf": {"compression": 32000},
    "lz4": hdf5plugin.LZ4(),
    "lz4hc": {"compression": 32004, "compression_opts": (0, 9)},
    "zstd": hdf5plugin.Zstd(clevel=3),
    "bshuf_pure": hdf5plugin.Bitshuffle(nelems=0, lz4=False),
    "bshuf_lz4": hdf5plugin.Bitshuffle(nelems=0, lz4=True),
    "zfp_prec": hdf5plugin.Zfp(precision=16),
    "zfp_rev": hdf5plugin.Zfp(reversible=True),
    "zfp_expert": hdf5plugin.Zfp(expert=(1, 16, 16, 0)),
    
    # Blosc1 explicit wrappers
    "blosc_lz": hdf5plugin.Blosc(cname='blosclz', clevel=5, shuffle=hdf5plugin.Blosc.SHUFFLE),
    "blosc_lz4": hdf5plugin.Blosc(cname='lz4', clevel=5, shuffle=hdf5plugin.Blosc.SHUFFLE),
    "blosc_lz4hc": hdf5plugin.Blosc(cname='lz4hc', clevel=5, shuffle=hdf5plugin.Blosc.SHUFFLE),
    "blosc_snappy": hdf5plugin.Blosc(cname='snappy', clevel=5, shuffle=hdf5plugin.Blosc.SHUFFLE),
    "blosc_zlib": hdf5plugin.Blosc(cname='zlib', clevel=5, shuffle=hdf5plugin.Blosc.SHUFFLE),
    "blosc_zstd": hdf5plugin.Blosc(cname='zstd', clevel=5, shuffle=hdf5plugin.Blosc.SHUFFLE),
    
    # Blosc2 explicit wrappers
    "blosc2_lz": hdf5plugin.Blosc2(cname='blosclz', clevel=5, shuffle=hdf5plugin.Blosc2.SHUFFLE),
    "blosc2_lz4": hdf5plugin.Blosc2(cname='lz4', clevel=5, shuffle=hdf5plugin.Blosc2.SHUFFLE),
    "blosc2_lz4hc": hdf5plugin.Blosc2(cname='lz4hc', clevel=5, shuffle=hdf5plugin.Blosc2.SHUFFLE),
    "blosc2_snappy": hdf5plugin.Blosc2(cname='snappy', clevel=5, shuffle=hdf5plugin.Blosc2.SHUFFLE),
    "blosc2_zlib": hdf5plugin.Blosc2(cname='zlib', clevel=5, shuffle=hdf5plugin.Blosc2.SHUFFLE),
    "blosc2_zstd": hdf5plugin.Blosc2(cname='zstd', clevel=5, shuffle=hdf5plugin.Blosc2.SHUFFLE),
    "blosc2_zfp": hdf5plugin.Blosc2(cname='zfp', clevel=5, shuffle=hdf5plugin.Blosc2.SHUFFLE),
    "blosc2_ndlz": hdf5plugin.Blosc2(cname='ndlz', clevel=5, shuffle=hdf5plugin.Blosc2.SHUFFLE)
}

with h5py.File('python_out.h5', 'w') as f:
    
    # --- 1. WRITE PHASE ---
    print("Writing datasets...")
    for name, opts in configs.items():
        dataset_data = data_int if name == "szip_nn" else data_float
        f.create_dataset(name, data=dataset_data, chunks=(128,), **opts)
        
    # --- 2. VERIFICATION PHASE ---
    print("\nVerifying raw DCPL filter attributes...")
    for name, opts in configs.items():
        ds = f[name]
        
        # Extract the raw creation property list (DCPL) to bypass h5py's string translations
        plist = ds.id.get_create_plist()
        
        # Assert a filter is actually applied
        if plist.get_nfilters() == 0:
            raise AssertionError(f"[{name}] FAILURE: No compression filters were applied to the dataset.")
        
        # Get the ID and Client Data values of the first filter in the pipeline
        filter_id, flags, values, filter_name = plist.get_filter(0)
        
        # Determine expected integer ID from our configuration dict
        # hdf5plugin objects provide a .filter_id attribute
        if hasattr(opts, 'filter_id'):
            expected_id = opts.filter_id
        elif 'compression' in opts:
            expected_comp = opts['compression']
            expected_id = expected_comp
            if expected_comp == 'gzip':
                expected_id = 1  # H5Z_FILTER_DEFLATE
            elif expected_comp == 'szip':
                expected_id = 4  # H5Z_FILTER_SZIP
            
        if filter_id != expected_id:
            raise AssertionError(f"[{name}] FAILURE: Expected Filter ID {expected_id}, but got {filter_id}")
            
        # Specific regression test: Verify Blosc/Blosc2 internal codecs (Snappy, LZ4, etc.)
        # Index 6 in the Blosc cd_values array holds the sub-compressor ID.
        if filter_id in (32001, 32026):
            if hasattr(opts, 'filter_options'):
                expected_compcode = opts.filter_options[6]
            else:
                expected_compcode = opts['compression_opts'][6] 
                
            actual_compcode = values[6]
            if actual_compcode != expected_compcode:
                raise AssertionError(f"[{name}] FAILURE: Expected Blosc compcode {expected_compcode}, but got {actual_compcode}")
        
        print(f"  [OK] {name.ljust(15)} -> Filter ID: {filter_id}")

print("\nPython successfully wrote and verified 'python_out.h5' with fully isolated states.")
