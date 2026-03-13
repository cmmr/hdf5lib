import h5py
import numpy as np
import hdf5plugin

names = [
    "zlibng_gzip", "szip_ec", "szip_nn", "bzip2", "lzf", "lz4", "lz4hc", "zstd",
    "bshuf_pure", "bshuf_lz4", "zfp_prec", "zfp_rev", "zfp_expert",
    "blosc_lz", "blosc_lz4", "blosc_lz4hc", "blosc_snappy", "blosc_zlib", "blosc_zstd",
    "blosc2_lz", "blosc2_lz4", "blosc2_lz4hc", "blosc2_snappy", "blosc2_zlib",
    "blosc2_zstd", "blosc2_zfp", "blosc2_ndlz"
]

with h5py.File('python_out.h5', 'r') as f_orig:
    with h5py.File('r_out.h5', 'r') as f_new:
        
        for i in range(27):
            next_i = (i + 1) % 27
            orig_name = names[i]
            new_name = "out_" + names[next_i]
            
            # Extract the originally generated array 
            # (which has precision constrained by the first write phase)
            expected = f_orig[orig_name][:]
            
            # Extract the C++ read/re-compressed array
            actual = f_new[new_name][:]
            
            # Assert they match. Use allclose to survive combinations involving lossy filters
            np.testing.assert_allclose(
                actual, expected, 
                atol=1e-2, rtol=1e-2, 
                err_msg=f"Data corruption detected during {orig_name} -> {new_name} transition."
            )

print("\nSUCCESS: All 27 inter-language HDF5 filter permutations completed a full round-trip.")
