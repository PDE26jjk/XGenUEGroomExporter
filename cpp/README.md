# xgcore

Compiled core for the XGen Groom Exporter: parses XGen `outSplineData` binary
and writes UE-compatible Alembic groom files from C++. The parser and Alembic
writer are verified byte-for-byte against the pure-Python reference on a real
sample (399,874 curves / 1,999,370 CVs per frame).

## Status

- Parser (blocks, JSON header, zlib groups, item decoding, UV strand order)
  - verified identical to the Python exporter (sorted and unsorted)
- Alembic writer (static + animated `OCurves` with widths and UE arb params)
  - verified by read-back with Alembic
- UE import verification is a manual developer step (see
  `docs/UE_Import_Checklist.md`)
- Python script integration is a follow-up change (the 4 existing `.py`
  scripts are intentionally untouched)

## Requirements

- Windows x64
- Visual Studio 2022 (MSVC) and CMake >= 3.24
- zlib 1.3.1, Imath 3.1.12, Alembic 1.8.12 sources (fetched automatically, or
  provided locally - see below)

## Build

### One-command build (this machine)

```powershell
powershell -ExecutionPolicy Bypass -File scripts/build.ps1
```

`scripts/build.ps1` loads the Visual Studio environment (vcvars64.bat), runs
CMake with the NMake Makefiles generator, and builds. Adjust the source
directories via `-AlembicSourceDir`, `-ImathSourceDir`, `-ZlibSourceDir`, and
`-VcVars` if your layout differs.

### Manual build

```bat
call R:\vs2022\VC\Auxiliary\Build\vcvars64.bat
cmake -S cpp -B cpp/build -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release ^
  -DALEMBIC_SOURCE_DIR=R:/git/alembic ^
  -DIMATH_SOURCE_DIR=<local Imath source> ^
  -DZLIB_SOURCE_DIR=<local zlib source>
cmake --build cpp/build
```

### Dependencies

The CMake project fetches zlib 1.3.1 and Imath 3.1.12 with FetchContent and
builds Alembic 1.8.12 statically (Ogawa only, no HDF5/Python bindings). All
libraries are linked statically with the MSVC static CRT (`/MT`), so
`xgcore.dll` has no runtime DLL dependencies beyond the OS (verified with
`dumpbin /dependents`: only KERNEL32.dll).

Offline builds: pass `-DALEMBIC_SOURCE_DIR=...`, `-DIMATH_SOURCE_DIR=...` and
`-DZLIB_SOURCE_DIR=...` pointing at local checkouts. The reference machine
keeps extracted copies under `cpp/third_party/` (gitignored).

## CLI Usage

```text
xgcore_test version
xgcore_test parse <raw.bin> [--sort] [--dump-dir <dir>]
xgcore_test read <file.abc>
xgcore_test abc <out.abc> <raw1.bin> [raw2.bin ...] [--animate]
             [--start <n>] [--fps <n>] [--sort] [--group <name>]
             [--guide] [--group-id <id>]
```

Each raw file becomes one frame (first raw = first frame). Repeating the same
raw file for every frame is allowed but produces a small file: Ogawa shares
identical arrays across samples, so an animated export only grows when the
per-frame data actually differs.

Examples:

```bat
xgcore_test parse raw_data.bin --sort --dump-dir out
xgcore_test abc hair_static.abc raw_data.bin --group hair1 --group-id 0
xgcore_test abc hair_anim.abc raw_data.bin raw_data.bin raw_data.bin --animate --sort --start 1 --fps 24
xgcore_test read hair_anim.abc
```

## Verification (A/B against the Python reference)

```bat
python scripts/reference_dump.py raw_data.bin ref_nosort
python scripts/reference_dump.py raw_data.bin ref_sort --sort
xgcore_test parse raw_data.bin --dump-dir dll_nosort
xgcore_test parse raw_data.bin --sort --dump-dir dll_sort
python scripts/ab_compare.py ref_nosort dll_nosort
python scripts/ab_compare.py ref_sort dll_sort
```

The harness compares counts, the UV permutation, curve offsets, positions,
widths, and root points byte-for-byte. Both modes pass on the reference
sample.

## C ABI

`include/xgcore/xgcore.h` is the public contract: plain C exports, integer
status codes, and a thread-local error message via `xg_last_error()`.
Because the DLL has no Python dependency, it can be loaded from any Python
generation through ctypes (Python 2.7 and 3.x):

```python
import ctypes

dll = ctypes.CDLL(r"path\to\xgcore.dll")
dll.xg_parse_frame_size.restype = ctypes.c_int


class XgParseInfo(ctypes.Structure):
    _fields_ = [("num_curves", ctypes.c_int64),
                ("num_cvs", ctypes.c_int64)]


info = XgParseInfo()
rc = dll.xg_parse_frame_size(raw_bytes, len(raw_bytes), 1, ctypes.byref(info))
if rc != 0:
    raise RuntimeError(dll.xg_last_error().decode("utf-8", "replace"))
```

See the header comments for buffer sizing and ordering semantics. A
Python-side wrapper with a pure-Python fallback is planned as a follow-up
change.

## Measured Performance

Reference sample: one frame with 399,874 curves / 1,999,370 CVs (32 MB raw,
~99 MB decompressed), x64 Release build on the reference machine:

| Operation | Time |
|---|---|
| Parse + UV sort + write static .abc (one frame) | ~1.2 s |
| Parse + UV sort (CLI, includes a second parse for sizing) | ~1.8 s |
| Animated export, per frame (same raw data repeated) | ~1.1 s/frame |

The previous pure-Python path spent ~10 s/frame on the same data
(`getXgenData` ~5 s + strand sort + imath fill ~5 s). The Maya-side
`writeBinary()` cost (~2.7 s/frame) is outside this project's scope.

## Limitations

- `GroupBase64=true` streams are rejected with a clear error (matches the
  current Python behavior).
- The parser assumes the fixed XGen header schema; JSON values must be
  integers/booleans/strings.
- Non-Windows builds and 32-bit builds are not supported.
- The per-frame vertex-count mismatch (strand counts changing between
  frames) is detected and reported, not yet repaired.

## License

See `NOTICE.md` for the licenses of statically linked third-party libraries
(Alembic, Imath, zlib).
