#!/usr/bin/env python3
"""Reference dump of the XGen spline-data parse + UV sort.

Reimplements the pure-Python logic used by XGenUEGroomExporter.py /
XGenDescriptionUEGroomExporter.py (getXgenData + get_index2order + output
assembly) without any Maya dependency, and writes the same arrays the xgcore
DLL produces. Used by ab_compare.py for exact A/B verification.

Usage:
  python reference_dump.py <raw.bin> <out_dir> [--sort]
"""

import argparse
import array
import json
import os
import struct
import sys
import zlib


def get_blocks(buf):
    blocks = []
    address = 0
    i = 0
    while address < len(buf) - 1:
        type_code = int.from_bytes(buf[address : address + 4], "little")
        size = int.from_bytes(buf[address + 8 : address + 16], "little")
        blocks.append((address + 16, address + 16 + size, type_code))
        address += size + 16
        i += 1
        if i > 100:
            break
    return blocks


def read_frame(raw):
    blocks = get_blocks(raw)
    if not blocks:
        raise ValueError("no blocks in raw data")
    header = json.loads(raw[blocks[0][0] : blocks[0][1]].decode("utf-8"))
    header_info = header["Header"]

    items = {}

    def collect(d):
        for k, v in d.items():
            if isinstance(v, int):
                g = v >> 32
                idx = v & 0xFFFFFFFF
                items.setdefault(k, []).append((g, idx))

    for entry in header["Items"]:
        collect(entry)
    for entry in header.get("RefMeshArray", []):
        collect(entry)

    groups = {}

    def group_bytes(g):
        if g not in groups:
            b = blocks[g + 1]
            if header_info.get("GroupDeflate"):
                groups[g] = zlib.decompress(raw[b[0] + 32 :])
            else:
                groups[g] = raw[b[0] : b[1]]
        return groups[g]

    def payload(key, g, idx):
        data = group_bytes(g)
        inner = get_blocks(data)
        return data[inner[idx][0] : inner[idx][1]]

    prim = []
    pos = []
    width = []
    face_id = []
    face_uv = []
    for g, idx in items.get("PrimitiveInfos", []):
        p = payload("PrimitiveInfos", g, idx)
        prim.append(
            [struct.unpack_from("<IQ", p, o) for o in range(0, len(p), 12)]
        )
    for g, idx in items.get("Positions", []):
        pos.append(array.array("f", payload("Positions", g, idx)))
    for g, idx in items.get("WIDTH_CV", []):
        width.append(array.array("f", payload("WIDTH_CV", g, idx)))
    for g, idx in items.get("FaceId", []):
        face_id.append(array.array("i", payload("FaceId", g, idx)))
    for g, idx in items.get("FaceUV", []):
        face_uv.append(array.array("f", payload("FaceUV", g, idx)))
    return prim, pos, width, face_id, face_uv


def assemble(prim, pos, width, face_id, face_uv, sort_by_uv):
    strands = []  # (cv_offset, length) for valid records, raw order
    raw_pos = array.array("f")
    raw_width = array.array("f")
    all_face_ids = array.array("i")
    all_face_uvs = array.array("f")

    if len(prim) != len(pos) or len(prim) != len(width):
        raise ValueError("PrimitiveInfos/Positions/WIDTH_CV segment count mismatch")
    for j in range(len(prim)):
        for offset, length in prim[j]:
            if length < 2:
                continue
            start = offset * 3
            raw_pos.extend(pos[j][start : start + length * 3])
            raw_width.extend(width[j][offset : offset + length])
            strands.append((len(raw_pos) // 3 - length, length))
        if face_id:
            all_face_ids.extend(face_id[j])
        if face_uv:
            all_face_uvs.extend(face_uv[j])

    num_curves = len(strands)
    num_cvs = len(raw_pos) // 3

    if sort_by_uv:
        if len(all_face_ids) != sum(len(p) for p in prim):
            raise ValueError("FaceId count mismatch")
        order_list = [
            (all_face_ids[i], all_face_uvs[2 * i], all_face_uvs[2 * i + 1])
            for i in range(len(all_face_ids))
        ]
        sorted_list = sorted((key, i) for i, key in enumerate(order_list))
        index_list = [0] * len(order_list)
        for order_index, item in enumerate(sorted_list):
            index_list[item[1]] = order_index
        order = index_list[:num_curves]
    else:
        order = list(range(num_curves))

    inv = [0] * num_curves
    for i, o in enumerate(order):
        inv[o] = i

    out_pos = array.array("f")
    out_width = array.array("f")
    out_roots = array.array("f")
    curve_offsets = [0]
    cursor = 0
    for o in range(num_curves):
        offset, length = strands[inv[o]]
        start = offset * 3
        out_pos.extend(raw_pos[start : start + length * 3])
        out_width.extend(raw_width[offset : offset + length])
        out_roots.extend(raw_pos[start : start + 3])
        cursor += length
        curve_offsets.append(cursor)

    if array.array("i").itemsize != 4:
        raise RuntimeError("platform int is not 32-bit")
    return {
        "meta": {"num_curves": num_curves, "num_cvs": num_cvs},
        "order": array.array("i", order),
        "curve_offsets": array.array("i", curve_offsets),
        "positions": out_pos,
        "widths": out_width,
        "root_points": out_roots,
    }


def write_bin(path, arr):
    with open(path, "wb") as f:
        f.write(arr.tobytes())


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("raw")
    parser.add_argument("out_dir")
    parser.add_argument("--sort", action="store_true")
    args = parser.parse_args()

    with open(args.raw, "rb") as f:
        raw = f.read()
    prim, pos, width, face_id, face_uv = read_frame(raw)
    result = assemble(prim, pos, width, face_id, face_uv, args.sort)

    os.makedirs(args.out_dir, exist_ok=True)
    with open(os.path.join(args.out_dir, "meta.json"), "w") as f:
        json.dump(result["meta"], f)
    write_bin(os.path.join(args.out_dir, "order.bin"), result["order"])
    write_bin(os.path.join(args.out_dir, "curve_offsets.bin"), result["curve_offsets"])
    write_bin(os.path.join(args.out_dir, "positions.bin"), result["positions"])
    write_bin(os.path.join(args.out_dir, "widths.bin"), result["widths"])
    write_bin(os.path.join(args.out_dir, "root_points.bin"), result["root_points"])
    print(
        "reference: %d curves, %d CVs -> %s"
        % (result["meta"]["num_curves"], result["meta"]["num_cvs"], args.out_dir)
    )


if __name__ == "__main__":
    sys.exit(main())
