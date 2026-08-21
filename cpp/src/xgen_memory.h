#ifndef XGCORE_XGEN_MEMORY_H
#define XGCORE_XGEN_MEMORY_H

#include <cstdint>

namespace xgcore {

struct ParsedFrame;

// Light probe for the direct-memory read route: verifies the MPxData ->
// XgSplineData pointer chain, the items-tree header, and that the stream
// accessor symbols resolve in AdskXGen.dll. Fills *ok and returns XG_OK
// when the probe executed (regardless of *ok); returns an XG_ERR_* code on
// internal failure.
int probe_memory_layout(uint64_t pdata_addr, bool& ok);

// Reads the live XgSplineData reachable from the MPxData at pdata_addr and
// fills `out` exactly like parse_frame does for the serialized bytes
// (including the sort_by_uv ordering). Returns XG_OK or an XG_ERR_* code.
int parse_memory_frame(uint64_t pdata_addr, bool sort_by_uv, ParsedFrame& out);

}  // namespace xgcore

#endif  // XGCORE_XGEN_MEMORY_H
