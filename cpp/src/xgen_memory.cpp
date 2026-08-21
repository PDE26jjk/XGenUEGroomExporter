#include "xgen_memory.h"

#include <cstdint>
#include <string>
#include <vector>

#ifndef _WIN32
#error "xgen_memory requires Windows (Maya runs on Windows x64)"
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "error.h"
#include "xgen_memory_internal.h"
#include "xgen_memory_seh.h"
#include "xgen_parse.h"
#include "xgcore/xgcore.h"

// All raw memory reads and AdskXGen.dll calls go through the SEH-guarded
// primitives in xgen_memory_seh.cpp; a bad pointer yields an error code
// instead of crashing the host (Maya) process.

namespace xgcore {
namespace {

// Offsets verified on Maya 2019/2025 by IDA reverse engineering:
//   MPxData + 0x10                  -> std::shared_ptr<XgSplineData>
//   XgSplineData + 0x10             -> items std::multimap (MSVC _Tree)
//   _Tree: _Myhead @ +0x00, _Mysize @ +0x08
//   _Tree node: left @ +0x00, parent @ +0x08, right @ +0x10, color @ +0x18,
//               key @ +0x20 (std::string, not needed), value @ +0x40
//   XgMappable<T> + 0x08            -> std::shared_ptr<XgVectorMappableImpl<T>>
//   XgVectorMappableImpl<T>: begin @ +0x08, end @ +0x10
constexpr uint64_t kMpxDataSplineOffset = 0x10;
constexpr uint64_t kSplineItemsOffset = 0x10;
constexpr uint64_t kTreeHeadOffset = 0x00;
constexpr uint64_t kTreeSizeOffset = 0x08;
constexpr uint64_t kNodeValueOffset = 0x40;
constexpr uint64_t kMappableImplOffset = 0x08;

// Stream accessors exported by AdskXGen.dll (identical symbol set on Maya
// 2019 and 2025). Each returns XgMappable<T>&; on x64 the reference comes
// back in RAX, so a free-function pointer with `this` as the first argument
// matches the calling convention.
struct StreamSymbols {
  void* (__cdecl* primitive_infos)(void*);
  void* (__cdecl* positions)(void*);
  void* (__cdecl* width_per_cv)(void*);
  void* (__cdecl* face_id)(void*);
  void* (__cdecl* face_uv)(void*);
};

int unsupported(const std::string& message) {
  set_error(message);
  return XG_ERR_UNSUPPORTED;
}

int malformed(const std::string& message) {
  set_error(message);
  return XG_ERR_MALFORMED;
}

std::string mappable_detail(const char* name, void* m) {
  char buf[1024];
  if (m == nullptr) {
    std::snprintf(buf, sizeof(buf), "%s mappable is null", name);
    return buf;
  }
  bool ok = false;
  const uint64_t impl =
      mem::guarded_read_u64(reinterpret_cast<uint64_t>(m) + 0x08, ok);
  uint64_t begin = 0;
  uint64_t end = 0;
  if (ok && impl != 0) {
    begin = mem::guarded_read_u64(impl + 0x08, ok);
    end = mem::guarded_read_u64(impl + 0x10, ok);
  }
  uint64_t q[8] = {0};
  if (impl != 0) {
    for (int i = 0; i < 8; ++i) {
      q[i] = mem::guarded_read_u64(impl + static_cast<uint64_t>(i) * 8, ok);
      if (!ok) break;
    }
  }
  std::snprintf(
      buf, sizeof(buf),
      "%s mappable=%p impl=%p begin=%p end=%p implq=[%llx %llx %llx %llx "
      "%llx %llx %llx %llx]",
      name, m, reinterpret_cast<void*>(impl), reinterpret_cast<void*>(begin),
      reinterpret_cast<void*>(end), static_cast<unsigned long long>(q[0]),
      static_cast<unsigned long long>(q[1]),
      static_cast<unsigned long long>(q[2]),
      static_cast<unsigned long long>(q[3]),
      static_cast<unsigned long long>(q[4]),
      static_cast<unsigned long long>(q[5]),
      static_cast<unsigned long long>(q[6]),
      static_cast<unsigned long long>(q[7]));
  return buf;
}

int resolve_symbols(StreamSymbols& out) {
  HMODULE mod = GetModuleHandleW(L"AdskXGen.dll");
  if (mod == nullptr) {
    return unsupported("AdskXGen.dll is not loaded in this process");
  }
  struct Symbol {
    const char* name;
    void* (__cdecl* * slot)(void*);
  };
  const Symbol symbols[] = {
      {"?primitiveInfos@XgSplineDataItem@@QEAAAEAV?$XgMappable@I@@XZ",
       &out.primitive_infos},
      {"?positions@XgSplineDataItem@@QEAAAEAV?$XgMappable@V?$SgVec3T@M@@@@XZ",
       &out.positions},
      {"?widthPerCV@XgSplineDataItem@@QEAAAEAV?$XgMappable@M@@XZ",
       &out.width_per_cv},
      {"?faceId@XgSplineDataItem@@QEAAAEAV?$XgMappable@I@@XZ",
       &out.face_id},
      {"?faceUV@XgSplineDataItem@@QEAAAEAV?$XgMappable@V?$SgVec2T@M@@@@XZ",
       &out.face_uv},
  };
  for (const Symbol& s : symbols) {
    const FARPROC proc = GetProcAddress(mod, s.name);
    if (proc == nullptr) {
      return unsupported(std::string("missing XGen symbol: ") + s.name);
    }
    *s.slot = reinterpret_cast<void* (__cdecl*)(void*)>(proc);
  }
  return XG_OK;
}

struct RawArray {
  const uint8_t* data = nullptr;
  size_t count = 0;  // element count
};

// Reads a live XgMappable<T> array (impl begin/end) for the given element
// size. Empty streams (begin == end == 0) are accepted.
bool read_mappable(void* mappable, size_t elem_size, RawArray& out) {
  out = RawArray();
  if (mappable == nullptr || !mem::readable(mappable, 0x20)) return false;
  bool ok = false;
  const uint64_t impl = mem::guarded_read_u64(
      reinterpret_cast<uint64_t>(mappable) + kMappableImplOffset, ok);
  if (!ok || impl == 0 ||
      !mem::readable(reinterpret_cast<const void*>(impl), 0x18)) {
    return false;
  }
  // The impl exposes its data through virtual slots (verified by
  // disassembly): slot 1 returns the element count, slot 4 the data pointer.
  // This works for both the AdskXGen.dll and xgenToolkit.mll impl classes.
  void* impl_ptr = reinterpret_cast<void*>(impl);
  const uint64_t count = mem::guarded_vcall_ret64(impl_ptr, 1, ok);
  if (!ok) return false;
  const uint64_t data = mem::guarded_vcall_ret64(impl_ptr, 4, ok);
  if (!ok) return false;
  out.count = static_cast<size_t>(count);
  if (out.count == 0) return true;
  if (data == 0 ||
      !mem::readable(reinterpret_cast<const void*>(data),
                     static_cast<size_t>(count) * elem_size)) {
    return false;
  }
  out.data = reinterpret_cast<const uint8_t*>(data);
  return true;
}

// Appends one item's strands to the frame, mirroring the serialized path's
// per-group handling: records with length < 2 are skipped, face arrays cover
// every record, and CVs are concatenated in raw order.
int read_item(const StreamSymbols& sym, uint64_t item, uint64_t& cv_cursor,
              ParsedFrame& out) {
  void* item_ptr = reinterpret_cast<void*>(item);
  bool ok = false;
  RawArray prim_a, pos_a, width_a, fid_a, fuv_a;
  void* m_prim = mem::guarded_call_accessor(sym.primitive_infos, item_ptr, ok);
  if (!ok) return malformed("primitiveInfos accessor call failed");
  if (!read_mappable(m_prim, 4, prim_a)) {
    return malformed(mappable_detail("primitiveInfos", m_prim));
  }
  void* m_pos = mem::guarded_call_accessor(sym.positions, item_ptr, ok);
  if (!ok) return malformed("positions accessor call failed");
  if (!read_mappable(m_pos, 12, pos_a)) {
    return malformed(mappable_detail("positions", m_pos));
  }
  void* m_width = mem::guarded_call_accessor(sym.width_per_cv, item_ptr, ok);
  if (!ok) return malformed("widthPerCV accessor call failed");
  if (!read_mappable(m_width, 4, width_a)) {
    return malformed("widthPerCV mappable read failed");
  }
  void* m_fid = mem::guarded_call_accessor(sym.face_id, item_ptr, ok);
  if (!ok) return malformed("faceId accessor call failed");
  if (!read_mappable(m_fid, 4, fid_a)) {
    return malformed("faceId mappable read failed");
  }
  void* m_fuv = mem::guarded_call_accessor(sym.face_uv, item_ptr, ok);
  if (!ok) return malformed("faceUV accessor call failed");
  if (!read_mappable(m_fuv, 8, fuv_a)) {
    return malformed("faceUV mappable read failed");
  }
  if (prim_a.count % 3 != 0) {
    return malformed("primitiveInfos must be (offset, length64) triples");
  }
  // In-memory PrimitiveInfos records are 3 uint32s each: offset, then the
  // 64-bit length split into low/high words (same 12-byte layout as the
  // serialized binary). Verified against live Maya 2025 data.
  const size_t records = prim_a.count / 3;
  if (fid_a.count != records || fuv_a.count != records) {
    char buf[128];
    std::snprintf(buf, sizeof(buf),
                  "FaceId/FaceUV count does not match PrimitiveInfos: "
                  "prim_count=%zu fid_count=%zu fuv_count=%zu",
                  prim_a.count, fid_a.count, fuv_a.count);
    return malformed(buf);
  }

  // Plain reads are safe here: read_mappable validated each whole array once
  // with VirtualQuery, and the strand data ranges are validated below (also
  // once) before copying.
  const uint32_t* prim = reinterpret_cast<const uint32_t*>(prim_a.data);
  const uint32_t* fid = reinterpret_cast<const uint32_t*>(fid_a.data);
  const float* fuv = reinterpret_cast<const float*>(fuv_a.data);
  const float* pos = reinterpret_cast<const float*>(pos_a.data);
  const float* width = reinterpret_cast<const float*>(width_a.data);

  // Pass 1: totals + face arrays (face arrays cover every record).
  uint64_t total_cvs = 0;
  for (size_t r = 0; r < records; ++r) {
    const uint64_t length = static_cast<uint64_t>(prim[3 * r + 1]) |
                            (static_cast<uint64_t>(prim[3 * r + 2]) << 32);
    ++out.total_records;
    out.face_ids.push_back(static_cast<int32_t>(fid[r]));
    out.face_uvs.push_back(fuv[2 * r]);
    out.face_uvs.push_back(fuv[2 * r + 1]);
    if (length >= 2) total_cvs += length;
  }

  // The slot-1 count can under-report the allocation on some impl classes,
  // so the records (authoritative, same source the serializer uses) drive
  // the validated range.
  if (total_cvs > 0) {
    if (!mem::readable(pos, static_cast<size_t>(total_cvs) * 3 *
                                sizeof(float))) {
      return malformed("strand position range exceeds the positions stream");
    }
    if (!mem::readable(width,
                       static_cast<size_t>(total_cvs) * sizeof(float))) {
      return malformed("strand width range exceeds the widthPerCV stream");
    }
  }

  // Pass 2: copy strands (plain reads within the validated ranges).
  const size_t pos_base = out.positions.size();
  const size_t width_base = out.widths.size();
  out.positions.resize(pos_base + static_cast<size_t>(total_cvs) * 3);
  out.widths.resize(width_base + static_cast<size_t>(total_cvs));
  uint64_t local_cursor = 0;
  for (size_t r = 0; r < records; ++r) {
    const uint64_t offset = prim[3 * r];
    const uint64_t length = static_cast<uint64_t>(prim[3 * r + 1]) |
                            (static_cast<uint64_t>(prim[3 * r + 2]) << 32);
    if (length < 2) continue;
    const size_t src_pos = static_cast<size_t>(offset) * 3;
    std::memcpy(out.positions.data() + pos_base +
                    static_cast<size_t>(local_cursor) * 3,
                pos + src_pos,
                static_cast<size_t>(length) * 3 * sizeof(float));
    std::memcpy(out.widths.data() + width_base +
                    static_cast<size_t>(local_cursor),
                width + static_cast<size_t>(offset),
                static_cast<size_t>(length) * sizeof(float));
    out.strands.push_back({static_cast<uint32_t>(cv_cursor), length});
    cv_cursor += length;
    local_cursor += length;
  }
  return XG_OK;
}

}  // namespace

int probe_memory_layout(uint64_t pdata_addr, bool& ok) {
  ok = false;
  if (pdata_addr == 0 ||
      !mem::readable(reinterpret_cast<const void*>(pdata_addr), 0x20)) {
    return XG_OK;
  }
  bool read_ok = false;
  const uint64_t spline =
      mem::guarded_read_u64(pdata_addr + kMpxDataSplineOffset, read_ok);
  if (!read_ok || spline == 0 ||
      !mem::readable(reinterpret_cast<const void*>(spline), 0x20)) {
    return XG_OK;
  }
  StreamSymbols sym;
  const int rc = resolve_symbols(sym);
  if (rc != XG_OK) return rc;
  const uint64_t head = mem::guarded_read_u64(
      spline + kSplineItemsOffset + kTreeHeadOffset, read_ok);
  if (!read_ok || head == 0 ||
      !mem::readable(reinterpret_cast<const void*>(head), 0x20)) {
    return XG_OK;
  }
  ok = true;
  return XG_OK;
}

int parse_memory_frame(uint64_t pdata_addr, bool sort_by_uv,
                       ParsedFrame& out) {
  out = ParsedFrame();
  if (pdata_addr == 0 ||
      !mem::readable(reinterpret_cast<const void*>(pdata_addr), 0x20)) {
    return malformed("invalid MPxData address");
  }
  bool read_ok = false;
  const uint64_t spline =
      mem::guarded_read_u64(pdata_addr + kMpxDataSplineOffset, read_ok);
  if (!read_ok || spline == 0 ||
      !mem::readable(reinterpret_cast<const void*>(spline), 0x20)) {
    return malformed("XgSplineData is null or unreachable");
  }
  StreamSymbols sym;
  int rc = resolve_symbols(sym);
  if (rc != XG_OK) return rc;
  const uint64_t items = spline + kSplineItemsOffset;
  if (!mem::readable(reinterpret_cast<const void*>(items), 0x10)) {
    return malformed("items tree header is unreachable");
  }
  const uint64_t head = mem::guarded_read_u64(items + kTreeHeadOffset, read_ok);
  if (!read_ok || head == 0 ||
      !mem::readable(reinterpret_cast<const void*>(head), 0x20)) {
    return malformed("items tree head is null or unreachable");
  }
  const uint64_t size = mem::guarded_read_u64(items + kTreeSizeOffset, read_ok);
  if (!read_ok || size == 0) {
    return malformed("XgSplineData contains no items");
  }

  // Collect item pointers with a guarded tree walk (SEH-protected, POD-only),
  // then decode each item. A layout mismatch surfaces as an error, never a
  // host crash.
  std::vector<uint64_t> item_ptrs(static_cast<size_t>(size));
  const size_t collected = mem::guarded_collect_items(
      reinterpret_cast<const uint8_t*>(head), size, item_ptrs.data(),
      item_ptrs.size());
  if (collected == SIZE_MAX || collected != item_ptrs.size()) {
    return malformed("items tree walk failed");
  }
  uint64_t cv_cursor = 0;
  for (const uint64_t item : item_ptrs) {
    if (item == 0 ||
        !mem::readable(reinterpret_cast<const void*>(item), 0x20)) {
      return malformed("XgSplineDataItem is null or unreachable");
    }
    rc = read_item(sym, item, cv_cursor, out);
    if (rc != XG_OK) return rc;
  }
  out.num_curves = static_cast<int64_t>(out.strands.size());
  out.num_cvs = static_cast<int64_t>(cv_cursor);
  sort_frame(out, sort_by_uv);
  return XG_OK;
}

}  // namespace xgcore
