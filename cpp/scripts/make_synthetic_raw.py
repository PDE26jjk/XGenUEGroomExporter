#!/usr/bin/env python3
"""Generates a small synthetic XgSplineData raw file for tests.

Three strands (3/4/2 CVs) with a frame-dependent position shift, stored in
the same block/deflate format as real outSplineData bytes.

Usage:
  python make_synthetic_raw.py <out.bin> <frame_index>
"""

import struct
import sys
import zlib


def block(payload, type_code=1):
    return struct.pack("<I4xQ", type_code, len(payload)) + payload


def f32(values):
    return struct.pack("<%df" % len(values), *values)


def i32(values):
    return struct.pack("<%di" % len(values), *values)


def build(frame):
    offsets = [0, 3, 7]
    lengths = [3, 4, 2]
    num_cvs = 9
    positions = [float(i) + frame * 0.5 for i in range(num_cvs * 3)]
    widths = [float(i) * 0.5 for i in range(num_cvs)]
    prim = b"".join(struct.pack("<IQ", o, l) for o, l in zip(offsets, lengths))
    face_ids = [2, 0, 1]
    face_uvs = [0.5, 0.1, 0.1, 0.9, 0.5, 0.1]
    group = (
        block(prim)
        + block(f32(positions))
        + block(f32(widths))
        + block(i32(face_ids))
        + block(f32(face_uvs))
    )
    header = (
        b'{"Header":{"Version":1,"Type":"XgSplineData","GroupVersion":1,'
        b'"GroupCount":1,"GroupBase64":false,"GroupDeflate":true,'
        b'"GroupDeflateLevel":9},'
        b'"Items":[{"Name":"x","Id":"{GUID}","PrimitiveInfos":0,'
        b'"Positions":1,"WIDTH_CV":2,"FaceId":3,"FaceUV":4}],'
        b'"RefMeshArray":[]}'
    )
    compressed = zlib.compress(group, 9)
    body = b"\x00" * 32 + compressed
    return block(header) + block(body, 2)


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 1
    out_path = sys.argv[1]
    frame = int(sys.argv[2])
    with open(out_path, "wb") as f:
        f.write(build(frame))
    print("wrote %s (frame %d)" % (out_path, frame))
    return 0


if __name__ == "__main__":
    sys.exit(main())
