"""h5py + hdf5plugin against hdf5lib's Blosc2 datasets (filter 32026).

Reads c_encoding_zoo.h5, as generate_c_zoo.c writes it, and for every dataset
that uses Blosc2:

  * checks cd_values: hdf5lib's packed settings in slot 0, and in slot 5 the
    last stage of the pre-filters they name;
  * describes it with hdf5plugin.from_filter_options();
  * appends to it through h5py (or, where axis 0 is fixed, writes new values
    over it), which compresses the new chunks with hdf5plugin's Blosc2 filter
    and hdf5lib's stored cd_values, then reads the old and new values back.

For the byte codecs (BloscLZ, LZ4, LZ4HC, Zlib, Zstd) the description must
match and the append must read back exactly: hdf5plugin runs only the one
stage cd_values[5] names, and all but trunc-prec are lossless.  It can't run
trunc-prec at all (it has no precision to give it), so where that is the
last stage, and for NDLZ, the ZFP codecs and a dataset whose ZFP codec
declined, the outcome is printed but not required.

Each dataset is checked in a copy of the file, so verify_c_zoo.py still sees
the original, and in a process of its own: a chunk the filter fails stays in
HDF5's cache and fails every flush after it, and h5py can crash over it.
Usage: python check_hdf5plugin_append.py [c_encoding_zoo.h5]
"""
import os
import shutil
import subprocess
import sys
import tempfile

import h5py
import hdf5plugin
import numpy as np

BLOSC2_ID = 32026
BYTE_CODECS = {0: "blosclz", 1: "lz4", 2: "lz4hc", 4: "zlib", 5: "zstd"}
FILTER_NAMES = {0: "none", 1: "shuffle", 2: "bitshuffle", 3: "delta", 4: "trunc-prec"}


def last_stage(mask):
    """The Blosc2 filter code of the last pre-filter in hdf5lib's bitmask."""
    if mask & 2:
        return 2
    if mask & 1:
        return 1
    if mask & 4:
        return 3
    if mask & 8:
        return 4
    return 0


def check_cd_values(cd, rank):
    """Problems with a list hdf5lib stored, or an empty list."""
    problems = []
    slot0 = cd[0]
    if slot0 >> 24 != 0xB2 or (slot0 >> 20) & 0xF != 1 or slot0 & 0xFF != 1:
        return [f"cd_values[0] = {slot0:#010x} is not hdf5lib's layout-1 settings"]
    mask = (slot0 >> 16) & 0xF
    if len(cd) < 7:
        problems.append(f"{len(cd)} values, not 7 or more")
    elif cd[5] != last_stage(mask):
        problems.append(f"cd_values[5] = {cd[5]}, not {last_stage(mask)} for bitmask {mask}")
    if len(cd) > 7 and (cd[7] < 2 or len(cd) != 8 + cd[7]):
        problems.append(f"{len(cd)} values with rank {cd[7]} in cd_values[7]")
    if len(cd) > 7 and cd[7] != rank and cd[6] != 32:  # NDLZ sees a 2-D byte image
        problems.append(f"rank {cd[7]} recorded for a rank-{rank} dataset")
    return problems


def new_block(dset, rows):
    """Values for `rows` more rows of dset, unlike the ones it holds."""
    shape = (rows,) + dset.shape[1:]
    n = int(np.prod(shape))
    if np.issubdtype(dset.dtype, np.floating):
        values = np.cos(np.arange(n) / 37.0) * 3.0 + 7.0
    else:
        values = (np.arange(n) * 7 + 3) % 30000
    return values.astype(dset.dtype).reshape(shape)


def check_one(src, name):
    """Checks one dataset in a copy of src; prints what happened, then RESULT."""
    workdir = tempfile.mkdtemp()
    path = os.path.join(workdir, "copy.h5")
    shutil.copyfile(src, path)
    problems = []

    with h5py.File(path, "r") as f:
        dset = f[name]
        cd = dset.id.get_create_plist().get_filter_by_id(BLOSC2_ID)[1]
        codec = cd[6] if len(cd) > 6 else 0
        required = codec in BYTE_CODECS and cd[5] != 4 and cd[4] <= 9
        print(f"\n[{name}] cd_values={tuple(cd)} ({'required' if required else 'recorded'})")
        problems += check_cd_values(cd, dset.ndim)

        # hdf5plugin's description
        try:
            desc = hdf5plugin.from_filter_options(BLOSC2_ID, tuple(cd))
            print(f"  describe: cname={desc.cname} clevel={desc.clevel} "
                  f"filters={desc.filters} ({FILTER_NAMES.get(desc.filters, '?')})")
            if desc.clevel != cd[4] or desc.filters != cd[5] or desc.cname != BYTE_CODECS.get(codec):
                problems.append("description does not match cd_values")
        except Exception as e:
            print(f"  describe: {type(e).__name__}: {e}")
            problems.append("hdf5plugin could not describe it")

        old = dset[...]
        extendable = dset.maxshape[0] is None
        start = old.shape[0] if extendable else 0
        rows = dset.chunks[0] if extendable else old.shape[0]
        block = new_block(dset, rows)
    action = "append" if extendable else "rewrite"

    # Append one chunk's worth of rows along the unlimited axis 0, or where
    # axis 0 is fixed, write new values over the whole dataset.  A failed
    # chunk stays in the cache and fails every flush after, so stop there.
    try:
        with h5py.File(path, "a") as f:
            dset = f[name]
            if extendable:
                dset.resize(old.shape[0] + rows, axis=0)
            dset[start:] = block
    except Exception as e:
        print(f"  {action}: {type(e).__name__}: {str(e).splitlines()[0]}")
        problems.append(f"h5py could not {action}")
        return finish(required, problems, workdir)

    # Read back through a fresh handle, so every chunk is decompressed
    try:
        with h5py.File(path, "r") as f:
            back = f[name][...]
        old_ok = np.array_equal(back[:start], old[:start])
        new_ok = np.array_equal(back[start:], block)
        diff = np.max(np.abs(back[start:].astype(np.float64) - block))
        print(f"  {action}: {rows} rows read back; old values intact: {old_ok}; "
              f"new values exact: {new_ok} (max diff {diff:g})")
        if not (old_ok and new_ok):
            problems.append(f"{action} did not read back exactly")
    except Exception as e:
        print(f"  read back: {type(e).__name__}: {str(e).splitlines()[0]}")
        problems.append("h5py could not read what it wrote")
    return finish(required, problems, workdir)


def finish(required, problems, workdir):
    if problems:
        print("  -> " + "; ".join(problems))
    status = "OK" if not problems else ("FAILED" if required else "RECORDED")
    print(f"RESULT {status}", flush=True)
    shutil.rmtree(workdir, ignore_errors=True)
    # HDF5 may still hold a chunk it could not flush; leave without closing it
    os._exit(0)


def main():
    if len(sys.argv) == 4 and sys.argv[1] == "--one":
        check_one(sys.argv[2], sys.argv[3])

    src = sys.argv[1] if len(sys.argv) > 1 else "c_encoding_zoo.h5"
    print("=" * 60)
    print("=== hdf5plugin: describe and append to hdf5lib's Blosc2 data ===")
    print("=" * 60)

    names = []
    with h5py.File(src, "r") as f:
        for name in sorted(f):
            if isinstance(f[name], h5py.Dataset):
                plist = f[name].id.get_create_plist()
                if any(plist.get_filter(i)[0] == BLOSC2_ID for i in range(plist.get_nfilters())):
                    names.append(name)

    # Each dataset in a process of its own (unbuffered, so that its output
    # survives a crash), which also keeps a crash from ending the run
    counts = {"OK": 0, "FAILED": 0, "RECORDED": 0}
    required = 0
    for name in names:
        run = subprocess.run([sys.executable, "-u", os.path.abspath(__file__), "--one", src, name],
                             capture_output=True, text=True)
        out = run.stdout.rstrip("\n").split("\n")
        status = out[-1].split()[1] if out and out[-1].startswith("RESULT ") else None
        is_required = "(required)" in run.stdout
        required += is_required
        print("\n".join(line for line in out if not line.startswith("RESULT ")))
        if status is None:
            stderr = run.stderr.strip().replace("\n", " | ")[-300:]
            print(f"  -> {'h5py crashed' if run.returncode < 0 else 'exited'} "
                  f"(exit {run.returncode}): {stderr}")
            status = "FAILED" if is_required or "(recorded)" not in run.stdout else "RECORDED"
        print(f"  {status}" + (" (not required)" if status == "RECORDED" else ""))
        counts[status] += 1

    print(f"\n=== {counts['OK']} OK, {counts['FAILED']} FAILED, "
          f"{counts['RECORDED']} recorded of {len(names)} Blosc2 datasets "
          f"({required} required) ===")
    sys.exit(1 if counts["FAILED"] or required == 0 else 0)


if __name__ == "__main__":
    main()
