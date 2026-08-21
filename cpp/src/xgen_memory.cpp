#include "xgen_memory.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#ifndef _WIN32
#error "xgen_memory requires Windows (Maya runs on Windows x64)"
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "error.h"
#include "xgen_memory_internal.h"
#include "xgen_parse.h"
#include "xgcore/xgcore.h"

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

bool readable(const void* p, size_t n) {
  if (p == nullptr || n == 0) return false;
  MEMORY_BASIC_INFORMATION info;
  if (VirtualQuery(p, &info, sizeof(info)) == 0) return false;
  if (info.State != MEM_COMMIT) return false;
  if (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
  const uintptr_t base = reinterpret_cast<uintptr_t>(info.BaseAddress);
  const uintptr_t end = base + info.RegionSize;
  const uintptr_t p_end = reinterpret_cast<uintptr_t>(p) + n;
  return p_end <= end;
}

uint64_t read_u64(uint64_t addr) {
  uint64_t v = 0;
  std::memcpy(&v, reinterpret_cast<const void*>(addr), sizeof(v));
  return v;
}

uint32_t read_u32(uint64_t addr) {
  uint32_t v = 0;
  std::memcpy(&v, reinterpret_cast<const void*>(addr), sizeof(v));
  return v;
}

float read_f32(uint64_t addr) {
  const uint32_t bits = read_u32(addr);
  float value = 0.0f;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

int unsupported(const std::string& message) {
  set_error(message);
  return XG_ERR_UNSUPPORTED;
}

int malformed(const std::string& message) {
  set_error(message);
  return XG_ERR_MALFORMED;
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
  if (mappable == nullptr || !readable(mappable, 0x20)) return false;
  const uint64_t impl =
      read_u64(reinterpret_cast<uint64_t>(mappable) + kMappableImplOffset);
  if (impl == 0 || !readable(reinterpret_cast<const void*>(impl), 0x18)) {
    return false;
  }
  const uint64_t begin = read_u64(impl + kImplBeginOffset);
  const uint64_t end = read_u64(impl + kImplEndOffset);
  if (end < begin) return false;
  const uint64_t bytes = end - begin;
  if (bytes % elem_size != 0) return false;
  out.count = static_cast<size_t>(bytes / elem_size);
  if (out.count == 0) return true;
  if (begin == 0 || !readable(reinterpret_cast<const void*>(begin),
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
  RawArray prim_a, pos_a, width_a, fid_a, fuv_a;
  if (!read_mappable(sym.primitive_infos(reinterpret_cast<void*>(item)), 4,
                     prim_a)) {
    return malformed("failed to read primitiveInfos stream");
  }
  if (!read_mappable(sym.positions(reinterpret_cast<void*>(item)), 12,
                     pos_a)) {
    return malformed("failed to read positions stream");
  }
  if (!read_mappable(sym.width_per_cv(reinterpret_cast<void*>(item)), 4,
                     width_a)) {
    return malformed("failed to read widthPerCV stream");
  }
  if (!read_mappable(sym.face_id(reinterpret_cast<void*>(item)), 4, fid_a)) {
    return malformed("failed to read faceId stream");
  }
  if (!read_mappable(sym.face_uv(reinterpret_cast<void*>(item)), 8, fuv_a)) {
    return malformed("failed to read faceUV stream");
  }
  if (prim_a.count % 2 != 0) {
    return malformed("primitiveInfos must be (offset, length) pairs");
  }
  const size_t records = prim_a.count / 2;
  if (fid_a.count != records || fuv_a.count != records) {
    return malformed("FaceId/FaceUV count does not match PrimitiveInfos");
  }
  for (size_t r = 0; r < records; ++r) {
    const uint64_t prim =
        reinterpret_cast<uint64_t>(prim_a.data) + r * 8;
    const uint32_t offset = read_u32(prim);
    const uint32_t length = read_u32(prim + 4);
    ++out.total_records;
    out.face_ids.push_back(
        static_cast<int32_t>(read_u32(reinterpret_cast<uint64_t>(fid_a.data) +
                                      r * 4)));
    const uint64_t uv = reinterpret_cast<uint64_t>(fuv_a.data) + r * 8;
    out.face_uvs.push_back(read_f32(uv));
    out.face_uvs.push_back(read_f32(uv + 4));
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
    out.positions.insert(out.positions.end(), pos_a.data + pos_byte,
                         pos_a.data + pos_byte + pos_bytes);
    const uint64_t width_byte = static_cast<uint64_t>(width_elem) * 4;
    const uint64_t width_bytes = static_cast<uint64_t>(length) * 4;
    out.widths.insert(out.widths.end(), width_a.data + width_byte,
                      width_a.data + width_byte + width_bytes);
    out.strands.push_back({static_cast<uint32_t>(cv_cursor), length});
    cv_cursor += length;
  }
  return XG_OK;
}

}  // namespace

int probe_memory_layout(uint64_t pdata_addr, bool& ok) {
  ok = false;
  if (pdata_addr == 0 ||
      !readable(reinterpret_cast<const void*>(pdata_addr), 0x20)) {
    return XG_OK;
  }
  const uint64_t spline = read_u64(pdata_addr + kMpxDataSplineOffset);
  if (spline == 0 ||
      !readable(reinterpret_cast<const void*>(spline), 0x20)) {
    return XG_OK;
  }
  StreamSymbols sym;
  const int rc = resolve_symbols(sym);
  if (rc != XG_OK) return rc;
  const uint64_t head = read_u64(spline + kSplineItemsOffset + kTreeHeadOffset);
  if (head == 0 || !readable(reinterpret_cast<const void*>(head), 0x20)) {
    return XG_OK;
  }
  ok = true;
  return XG_OK;
}

int parse_memory_frame(uint64_t pdata_addr, bool sort_by_uv,
                       ParsedFrame& out) {
  out = ParsedFrame();
  if (pdata_addr == 0 ||
      !readable(reinterpret_cast<const void*>(pdata_addr), 0x20)) {
    return malformed("invalid MPxData address");
  }
  const uint64_t spline = read_u64(pdata_addr + kMpxDataSplineOffset);
  if (spline == 0 ||
      !readable(reinterpret_cast<const void*>(spline), 0x20)) {
    return malformed("XgSplineData is null or unreachable");
  }
  StreamSymbols sym;
  int rc = resolve_symbols(sym);
  if (rc != XG_OK) return rc;
  const uint64_t items = spline + kSplineItemsOffset;
  if (!readable(reinterpret_cast<const void*>(items), 0x10)) {
    return malformed("items tree header is unreachable");
  }
  const uint64_t head = read_u64(items + kTreeHeadOffset);
  const uint64_t size = read_u64(items + kTreeSizeOffset);
  if (head == 0 ||
      !readable(reinterpret_cast<const void*>(head), 0x20)) {
    return malformed("items tree head is null or unreachable");
  }
  if (size == 0) return malformed("XgSplineData contains no items");
  const uint8_t* node =
      mem::tree_leftmost(reinterpret_cast<const uint8_t*>(head));
  if (node == nullptr) return malformed("items tree is empty");
  uint64_t cv_cursor = 0;
  for (uint64_t i = 0; i < size; ++i) {
    if (node == reinterpret_cast<const uint8_t*>(head) ||
        !readable(node, 0x48)) {
      return malformed("items tree walk failed");
    }
    const uint64_t item =
        read_u64(reinterpret_cast<uint64_t>(node) + kNodeValueOffset);
    if (item == 0 ||
        !readable(reinterpret_cast<const void*>(item), 0x20)) {
      return malformed("XgSplineDataItem is null or unreachable");
    }
    rc = read_item(sym, item, cv_cursor, out);
    if (rc != XG_OK) return rc;
    node = mem::tree_next(node, reinterpret_cast<const uint8_t*>(head));
  }
  out.num_curves = static_cast<int64_t>(out.strands.size());
  out.num_cvs = static_cast<int64_t>(cv_cursor);
  sort_frame(out, sort_by_uv);
  return XG_OK;
}

}  // namespace xgcore
