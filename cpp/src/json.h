#ifndef XGCORE_JSON_H
#define XGCORE_JSON_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace xgcore {

class JsonError : public std::runtime_error {
 public:
  explicit JsonError(const std::string& what) : std::runtime_error(what) {}
};

// Minimal JSON value tree covering the fixed XGen header schema
// (objects, arrays, strings, integers, booleans, null).
class Json {
 public:
  enum class Type { Null, Bool, Int, String, Array, Object };

  Json() = default;

  static Json parse(const std::string& text);  // throws JsonError on failure

  Type type() const { return type_; }
  bool is_null() const { return type_ == Type::Null; }
  bool is_bool() const { return type_ == Type::Bool; }
  bool is_int() const { return type_ == Type::Int; }
  bool is_string() const { return type_ == Type::String; }
  bool is_array() const { return type_ == Type::Array; }
  bool is_object() const { return type_ == Type::Object; }

  bool as_bool() const;
  int64_t as_int() const;
  const std::string& as_string() const;
  const std::vector<Json>& as_array() const;
  const std::map<std::string, Json>& as_object() const;

  // Object member lookup; returns nullptr when missing or not an object.
  const Json* find(const std::string& key) const;

  // Factories used by the parser.
  static Json make_bool(bool value);
  static Json make_int(int64_t value);
  static Json make_string(std::string value);
  static Json make_array();
  static Json make_object();

  // Mutation used by the parser while building trees.
  void set(const std::string& key, Json value);
  void append(Json value);

 private:
  Type type_ = Type::Null;
  bool bool_ = false;
  int64_t int_ = 0;
  std::string str_;
  std::vector<Json> array_;
  std::map<std::string, Json> object_;
};

}  // namespace xgcore

#endif  // XGCORE_JSON_H
