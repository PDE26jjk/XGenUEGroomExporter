#ifndef XGCORE_ERROR_H
#define XGCORE_ERROR_H

#include <string>

namespace xgcore {

// Thread-local error state, surfaced through xg_last_error().
void set_error(std::string message);
void clear_error();
const char* last_error();

}  // namespace xgcore

#endif  // XGCORE_ERROR_H

