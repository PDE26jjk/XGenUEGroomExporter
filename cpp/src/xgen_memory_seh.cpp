#include "xgen_memory_seh.h"

#include <cstring>

#ifndef _WIN32
#error "xgen_memory_seh requires Windows"
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "xgen_memory_internal.h"

namespace xgcore {
namespace mem {

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

uint64_t guarded_read_u64(uint64_t addr, bool& ok) {
  ok = false;
  if (!readable(reinterpret_cast<const void*>(addr), 8)) return 0;
  __try {
    uint64_t v = 0;
    std::memcpy(&v, reinterpret_cast<const void*>(addr), sizeof(v));
    ok = true;
    return v;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    ok = false;
    return 0;
  }
}

uint32_t guarded_read_u32(uint64_t addr, bool& ok) {
  ok = false;
  if (!readable(reinterpret_cast<const void*>(addr), 4)) return 0;
  __try {
    uint32_t v = 0;
    std::memcpy(&v, reinterpret_cast<const void*>(addr), sizeof(v));
    ok = true;
    return v;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    ok = false;
    return 0;
  }
}

float guarded_read_f32(uint64_t addr, bool& ok) {
  const uint32_t bits = guarded_read_u32(addr, ok);
  float value = 0.0f;
  if (ok) std::memcpy(&value, &bits, sizeof(value));
  return value;
}

bool guarded_read_bytes(uint64_t addr, void* out, size_t n) {
  if (out == nullptr || !readable(reinterpret_cast<const void*>(addr), n)) {
    return false;
  }
  __try {
    std::memcpy(out, reinterpret_cast<const void*>(addr), n);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

void* guarded_call_accessor(void* (__cdecl* fn)(void*), void* self, bool& ok) {
  ok = false;
  if (fn == nullptr) return nullptr;
  __try {
    void* result = fn(self);
    ok = true;
    return result;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    ok = false;
    return nullptr;
  }
}

uint64_t guarded_vcall_ret64(void* obj, int slot, bool& ok) {
  ok = false;
  if (obj == nullptr || slot < 0 || !readable(obj, 8)) return 0;
  __try {
    void** vtable = *reinterpret_cast<void***>(obj);
    if (vtable == nullptr ||
        !readable(vtable, static_cast<size_t>(slot + 1) * 8)) {
      return 0;
    }
    uint64_t (__cdecl* fn)(void*) =
        reinterpret_cast<uint64_t (__cdecl*)(void*)>(vtable[slot]);
    if (fn == nullptr) return 0;
    const uint64_t result = fn(obj);
    ok = true;
    return result;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    ok = false;
    return 0;
  }
}

size_t guarded_collect_items(const uint8_t* head, uint64_t size, uint64_t* out,
                             size_t capacity) {
  if (head == nullptr || out == nullptr || capacity < size) {
    return SIZE_MAX;
  }
  __try {
    const uint8_t* node = tree_leftmost(head);
    if (node == nullptr) return SIZE_MAX;
    for (uint64_t i = 0; i < size; ++i) {
      if (node == head || !readable(node, 0x48)) return SIZE_MAX;
      out[i] = *reinterpret_cast<const uint64_t*>(node + 0x40);
      node = tree_next(node, head);
    }
    return static_cast<size_t>(size);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return SIZE_MAX;
  }
}

}  // namespace mem
}  // namespace xgcore
