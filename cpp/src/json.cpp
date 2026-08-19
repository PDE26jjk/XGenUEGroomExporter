#include "json.h"

#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace xgcore {

namespace {

class Parser {
 public:
  explicit Parser(const std::string& text) : text_(text) {}

  Json parse() {
    Json value = parse_value();
    skip_ws();
    if (pos_ != text_.size()) {
      throw JsonError("trailing characters after JSON value");
    }
    return value;
  }

 private:
  const std::string& text_;
  size_t pos_ = 0;

  void skip_ws() {
    while (pos_ < text_.size() &&
           (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\n' ||
            text_[pos_] == '\r')) {
      ++pos_;
    }
  }

  char peek() {
    if (pos_ >= text_.size()) throw JsonError("unexpected end of input");
    return text_[pos_];
  }

  char take() {
    char c = peek();
    ++pos_;
    return c;
  }

  void expect(char c) {
    if (take() != c) throw JsonError(std::string("expected '") + c + "'");
  }

  Json parse_value() {
    skip_ws();
    char c = peek();
    switch (c) {
      case '{': return parse_object();
      case '[': return parse_array();
      case '"': return Json::make_string(parse_string());
      case 't':
        expect_literal("true");
        return Json::make_bool(true);
      case 'f':
        expect_literal("false");
        return Json::make_bool(false);
      case 'n':
        expect_literal("null");
        return Json();
      default:
        if (c == '-' || (c >= '0' && c <= '9')) return parse_number();
        throw JsonError("unexpected character in JSON value");
    }
  }

  void expect_literal(const char* lit) {
    for (const char* p = lit; *p; ++p) {
      if (take() != *p) throw JsonError("invalid literal");
    }
  }

  Json parse_object() {
    Json obj = Json::make_object();
    expect('{');
    skip_ws();
    if (peek() == '}') {
      ++pos_;
      return obj;
    }
    while (true) {
      skip_ws();
      if (peek() != '"') throw JsonError("expected string key");
      std::string key = parse_string();
      skip_ws();
      expect(':');
      obj.set(key, parse_value());
      skip_ws();
      char c = take();
      if (c == ',') continue;
      if (c == '}') break;
      throw JsonError("expected ',' or '}' in object");
    }
    return obj;
  }

  Json parse_array() {
    Json arr = Json::make_array();
    expect('[');
    skip_ws();
    if (peek() == ']') {
      ++pos_;
      return arr;
    }
    while (true) {
      arr.append(parse_value());
      skip_ws();
      char c = take();
      if (c == ',') continue;
      if (c == ']') break;
      throw JsonError("expected ',' or ']' in array");
    }
    return arr;
  }

  Json parse_number() {
    size_t start = pos_;
    if (peek() == '-') ++pos_;
    while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
      ++pos_;
    }
    // Fraction/exponent parts are not expected in the XGen header; reject
    // them explicitly instead of silently truncating.
    if (pos_ < text_.size() &&
        (text_[pos_] == '.' || text_[pos_] == 'e' || text_[pos_] == 'E')) {
      throw JsonError("floating point numbers are not supported");
    }
    std::string token = text_.substr(start, pos_ - start);
    if (token.empty() || token == "-") throw JsonError("invalid number");
    errno = 0;
    char* end = nullptr;
    long long value = std::strtoll(token.c_str(), &end, 10);
    if (errno != 0 || end == token.c_str()) throw JsonError("invalid number");
    return Json::make_int(static_cast<int64_t>(value));
  }

  std::string parse_string() {
    expect('"');
    std::string out;
    while (true) {
      char c = take();
      if (c == '"') break;
      if (c == '\\') {
        char e = take();
        switch (e) {
          case '"': out.push_back('"'); break;
          case '\\': out.push_back('\\'); break;
          case '/': out.push_back('/'); break;
          case 'b': out.push_back('\b'); break;
          case 'f': out.push_back('\f'); break;
          case 'n': out.push_back('\n'); break;
          case 'r': out.push_back('\r'); break;
          case 't': out.push_back('\t'); break;
          case 'u': {
            unsigned cp = parse_hex4();
            append_utf8(out, cp);
            break;
          }
          default: throw JsonError("invalid escape sequence");
        }
      } else if (static_cast<unsigned char>(c) < 0x20) {
        throw JsonError("control character in string");
      } else {
        out.push_back(c);
      }
    }
    return out;
  }

  unsigned parse_hex4() {
    unsigned value = 0;
    for (int i = 0; i < 4; ++i) {
      char c = take();
      value <<= 4;
      if (c >= '0' && c <= '9') value |= static_cast<unsigned>(c - '0');
      else if (c >= 'a' && c <= 'f') value |= static_cast<unsigned>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') value |= static_cast<unsigned>(c - 'A' + 10);
      else throw JsonError("invalid \\u escape");
    }
    return value;
  }

  static void append_utf8(std::string& out, unsigned cp) {
    if (cp < 0x80) {
      out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }
};

}  // namespace

Json Json::parse(const std::string& text) { return Parser(text).parse(); }

bool Json::as_bool() const {
  if (type_ != Type::Bool) throw JsonError("not a boolean");
  return bool_;
}

int64_t Json::as_int() const {
  if (type_ != Type::Int) throw JsonError("not an integer");
  return int_;
}

const std::string& Json::as_string() const {
  if (type_ != Type::String) throw JsonError("not a string");
  return str_;
}

const std::vector<Json>& Json::as_array() const {
  if (type_ != Type::Array) throw JsonError("not an array");
  return array_;
}

const std::map<std::string, Json>& Json::as_object() const {
  if (type_ != Type::Object) throw JsonError("not an object");
  return object_;
}

const Json* Json::find(const std::string& key) const {
  if (type_ != Type::Object) return nullptr;
  auto it = object_.find(key);
  return it == object_.end() ? nullptr : &it->second;
}

Json Json::make_bool(bool value) {
  Json j;
  j.type_ = Type::Bool;
  j.bool_ = value;
  return j;
}

Json Json::make_int(int64_t value) {
  Json j;
  j.type_ = Type::Int;
  j.int_ = value;
  return j;
}

Json Json::make_string(std::string value) {
  Json j;
  j.type_ = Type::String;
  j.str_ = std::move(value);
  return j;
}

Json Json::make_array() {
  Json j;
  j.type_ = Type::Array;
  return j;
}

Json Json::make_object() {
  Json j;
  j.type_ = Type::Object;
  return j;
}

void Json::set(const std::string& key, Json value) {
  type_ = Type::Object;
  object_[key] = std::move(value);
}

void Json::append(Json value) {
  type_ = Type::Array;
  array_.push_back(std::move(value));
}

}  // namespace xgcore
