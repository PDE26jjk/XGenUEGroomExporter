#include "error.h"

#include <mutex>
#include <string>

namespace xgcore {
namespace {

thread_local std::string g_error;

}  // namespace

void set_error(std::string message) { g_error = std::move(message); }
void clear_error() { g_error.clear(); }
const char* last_error() { return g_error.c_str(); }

}  // namespace xgcore

