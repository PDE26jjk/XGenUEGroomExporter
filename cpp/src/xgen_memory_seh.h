#ifndef XGCORE_XGEN_MEMORY_SEH_H
#define XGCORE_XGEN_MEMORY_SEH_H

#include <cstddef>
#include <cstdint>

// SEH-guarded primitives for the direct-memory reader. Compiled in a
// dedicated translation unit with /EHa so __try/__except is valid (the
// functions only touch POD locals, never C++ objects requiring unwinding).
// Every function converts an access violation into a failure result instead
// of crashing the host process.
namespace xgcore {
namespace mem {

bool readable(const void* p, size_t n);

// Reads are only attempted after readable() confirms the range is committed
// and readable; SEH catches any remaining fault.
uint64_t guarded_read_u64(uint64_t addr, bool& ok);
uint32_t guarded_read_u32(uint64_t addr, bool& ok);
float guarded_read_f32(uint64_t addr, bool& ok);
bool guarded_read_bytes(uint64_t addr, void* out, size_t n);

// Calls a foreign (AdskXGen.dll) stream accessor with SEH protection.
void* guarded_call_accessor(void* (__cdecl* fn)(void*), void* self, bool& ok);

// Calls virtual slot `slot` on a polymorphic object (reads the vtable, then
// calls the slot with `this` in RCX) with SEH protection. Returns the RAX
// result (pointer or integer). Fails (ok=false) on any access violation.
uint64_t guarded_vcall_ret64(void* obj, int slot, bool& ok);

// Walks the items _Tree in-order and writes each node's value pointer into
// `out`. Returns the number of items collected, or SIZE_MAX on any access
// violation / structural failure.
size_t guarded_collect_items(const uint8_t* head, uint64_t size, uint64_t* out,
                             size_t capacity);

}  // namespace mem
}  // namespace xgcore

#endif  // XGCORE_XGEN_MEMORY_SEH_H
