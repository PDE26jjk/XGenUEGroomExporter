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
constexpr uint64_t kImplBeginOffset = 0x08;
constexpr uint64_t kImplEndOffset = 0x10;

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
  const uint64_t begin = mem::guarded_read_u64(impl + kImplBeginOffset, ok);
  if (!ok) return false;
  const uint64_t end = mem::guarded_read_u64(impl + kImplEndOffset, ok);
  if (!ok || end < begin) return false;
  const uint64_t bytes = end - begin;
  if (bytes % elem_size != 0) return false;
  out.count = static_cast<size_t>(bytes / elem_size);
  if (out.count == 0) return true;
  if (begin == 0 || !mem::readable(reinterpret_cast<const void*>(begin),
                                   static_cast<size_t>(bytes))) {
    return false;
  }
  out.data = reinterpret_cast<const uint8_t*>(begin);
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
  if (prim_a.count % 2 != 0) {
    return malformed("primitiveInfos must be (offset, length) pairs");
  }
  const size_t records = prim_a.count / 2;
  if (fid_a.count != records || fuv_a.count != records) {
    return malformed("FaceId/FaceUV count does not match PrimitiveInfos");
  }
  for (size_t r = 0; r < records; ++r) {
    const uint64_t prim = reinterpret_cast<uint64_t>(prim_a.data) + r * 8;
    const uint32_t offset = mem::guarded_read_u32(prim, ok);
    if (!ok) return malformed("failed to read primitiveInfos record");
    const uint32_t length = mem::guarded_read_u32(prim + 4, ok);
    if (!ok) return malformed("failed to read primitiveInfos record");
    ++out.total_records;
    const uint32_t face_id =
        mem::guarded_read_u32(reinterpret_cast<uint64_t>(fid_a.data) + r * 4,
                              ok);
    if (!ok) return malformed("failed to read faceId record");
    out.face_ids.push_back(static_cast<int32_t>(face_id));
    const uint64_t uv = reinterpret_cast<uint64_t>(fuv_a.data) + r * 8;
    const float u = mem::guarded_read_f32(uv, ok);
    if (!ok) return malformed("failed to read faceUV record");
    const float v = mem::guarded_read_f32(uv + 4, ok);
    if (!ok) return malformed("failed to read faceUV record");
    out.face_uvs.push_back(u);
    out.face_uvs.push_back(v);
    if (length < 2) continue;
    const size_t pos_elem = static_cast<size_t>(offset) * 3;
    if (pos_elem + static_cast<size_t>(length) * 3 > pos_a.count) {
      return malformed("strand position range exceeds the positions stream");
    }
    const size_t width_elem = static_cast<size_t>(offset);
    if (width_elem + static_cast<size_t>(length) > width_a.count) {
      return malformed("strand width range exceeds the widthPerCV stream");
    }
    const uint64_t pos_byte = static_cast<uint64_t>(pos_elem) * 4;
    const uint64_t pos_bytes = static_cast<uint64_t>(length) * 12;
    const size_t old_pos = out.positions.size();
    out.positions.resize(old_pos + static_cast<size_t>(length) * 3);
    if (!mem::guarded_read_bytes(
            reinterpret_cast<uint64_t>(pos_a.data) + pos_byte,
            out.positions.data() + old_pos, static_cast<size_t>(pos_bytes))) {
      return malformed("failed to copy positions");
    }
    const uint64_t width_byte = static_cast<uint64_t>(width_elem) * 4;
    const uint64_t width_bytes = static_cast<uint64_t>(length) * 4;
    const size_t old_widths = out.widths.size();
    out.widths.resize(old_widths + static_cast<size_t>(length));
    if (!mem::guarded_read_bytes(reinterpret_cast<uint64_t>(width_a.data) +
                                     width_byte,
                                 out.widths.data() + old_widths,
                                 static_cast<size_t>(width_bytes))) {
      return malformed("failed to copy widths");
    }
    out.strands.push_back({static_cast<uint32_t>(cv_cursor), length});
    cv_cursor += length;
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
