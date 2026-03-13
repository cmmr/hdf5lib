import h5py
import numpy as np
import hdf5plugin

# 1024 size enables 128-element chunks, maintaining divisible-by-8 constraints for szip
data_float = np.random.rand(1024).astype(np.float64)

# Map the 27 smoke_test.c combinations directly to h5py's C-API passthrough tuples
# (Note: Standalone 'snappy' / ID 32003 is omitted as it is not bundled by hdf5plugin)
configs = {
    "zlibng_gzip": {"compression": "gzip", "compression_opts": 9},
    "szip_ec": {"compression": "szip", "compression_opts": ("ec", 8)},
    "szip_nn": {"compression": "szip", "compression_opts": ("nn", 8)},
    "bzip2": {"compression": 307, "compression_opts": (9,)},
    "lzf": {"compression": 32000, "compression_opts": None},
    "lz4": {"compression": 32004, "compression_opts": (0, 0)},
    "lz4hc": {"compression": 32004, "compression_opts": (0, 9)},
    "zstd": {"compression": 32015, "compression_opts": (3,)},
    "bshuf_pure": {"compression": 32008, "compression_opts": (0, 0)},
    "bshuf_lz4": {"compression": 32008, "compression_opts": (0, 2)},
    "zfp_prec": {"compression": 32013, "compression_opts": (2, 0, 16, 0, 0, 0)},
    "zfp_rev": {"compression": 32013, "compression_opts": (5, 0, 0, 0, 0, 0)},
    "zfp_expert": {"compression": 32013, "compression_opts": (4, 0, 1, 16, 16, 0)},
    "blosc_lz": {"compression": 32001, "compression_opts": (0, 0, 0, 0, 5, 1, 0)},
    "blosc_lz4": {"compression": 32001, "compression_opts": (0, 0, 0, 0, 5, 1, 1)},
    "blosc_lz4hc": {"compression": 32001, "compression_opts": (0, 0, 0, 0, 5, 1, 2)},
    "blosc_snappy": {"compression": 32001, "compression_opts": (0, 0, 0, 0, 5, 1, 3)},
    "blosc_zlib": {"compression": 32001, "compression_opts": (0, 0, 0, 0, 5, 1, 4)},
    "blosc_zstd": {"compression": 32001, "compression_opts": (0, 0, 0, 0, 5, 1, 5)},
    "blosc2_lz": {"compression": 32026, "compression_opts": (0, 0, 0, 0, 5, 1, 0)},
    "blosc2_lz4": {"compression": 32026, "compression_opts": (0, 0, 0, 0, 5, 1, 1)},
    "blosc2_lz4hc": {"compression": 32026, "compression_opts": (0, 0, 0, 0, 5, 1, 2)},
    "blosc2_snappy": {"compression": 32026, "compression_opts": (0, 0, 0, 0, 5, 1, 3)},
    "blosc2_zlib": {"compression": 32026, "compression_opts": (0, 0, 0, 0, 5, 1, 4)},
    "blosc2_zstd": {"compression": 32026, "compression_opts": (0, 0, 0, 0, 5, 1, 5)},
    "blosc2_zfp": {"compression": 32026, "compression_opts": (0, 0, 0, 0, 5, 1, 6)},
    "blosc2_ndlz": {"compression": 32026, "compression_opts": (0, 0, 0, 0, 5, 1, 11)}
}

with h5py.File('python_out.h5', 'w') as f:
    for name, opts in configs.items():
        opts_copy = dict(opts)
        if opts_copy["compression_opts"] is None:
            del opts_copy["compression_opts"]
        f.create_dataset(name, data=data_float, chunks=(128,), **opts_copy)

print("Python successfully wrote 'python_out.h5' with 27 filter permutations.")
