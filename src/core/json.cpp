#include "localsend/json.hpp"

#include <cmath>
#include <iomanip>
#include <sstream>

namespace localsend {
namespace {

class Parser {
public:
  explicit Parser(const std::string& text) : text_(text) {}

  Json parse() {
    skip_ws();
    Json value = parse_value();
    skip_ws();
    if (pos_ != text_.size()) {
      throw JsonError("unexpected trailing JSON data");
    }
    return value;
  }

private:
  Json parse_value() {
    if (pos_ >= text_.size()) {
      throw JsonError("unexpected end of JSON");
    }

    const char c = text_[pos_];
    if (c == 'n') {
      expect("null");
      return Json(nullptr);
    }
    if (c == 't') {
      expect("true");
      return Json(true);
    }
    if (c == 'f') {
      expect("false");
      return Json(false);
    }
    if (c == '"') {
      return Json(parse_string());
    }
    if (c == '{') {
      return parse_object();
    }
    if (c == '[') {
      return parse_array();
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
      return parse_number();
    }
    throw JsonError("invalid JSON value");
  }

  Json parse_object() {
    consume('{');
    Json::Object object;
    skip_ws();
    if (try_consume('}')) {
      return Json(std::move(object));
    }

    while (true) {
      skip_ws();
      if (peek() != '"') {
        throw JsonError("expected object key");
      }
      std::string key = parse_string();
      skip_ws();
      consume(':');
      skip_ws();
      object.emplace(std::move(key), parse_value());
      skip_ws();
      if (try_consume('}')) {
        break;
      }
      consume(',');
    }

    return Json(std::move(object));
  }

  Json parse_array() {
    consume('[');
    Json::Array array;
    skip_ws();
    if (try_consume(']')) {
      return Json(std::move(array));
    }

    while (true) {
      skip_ws();
      array.emplace_back(parse_value());
      skip_ws();
      if (try_consume(']')) {
        break;
      }
      consume(',');
    }

    return Json(std::move(array));
  }

  Json parse_number() {
    const std::size_t begin = pos_;
    if (peek() == '-') {
      ++pos_;
    }
    if (peek() == '0') {
      ++pos_;
    } else {
      consume_digits();
    }
    if (peek() == '.') {
      ++pos_;
      consume_digits();
    }
    if (peek() == 'e' || peek() == 'E') {
      ++pos_;
      if (peek() == '+' || peek() == '-') {
        ++pos_;
      }
      consume_digits();
    }

    try {
      return Json(std::stod(text_.substr(begin, pos_ - begin)));
    } catch (const std::exception&) {
      throw JsonError("invalid JSON number");
    }
  }

  std::string parse_string() {
    consume('"');
    std::string out;
    while (pos_ < text_.size()) {
      const char c = text_[pos_++];
      if (c == '"') {
        return out;
      }
      if (static_cast<unsigned char>(c) < 0x20) {
        throw JsonError("control character in JSON string");
      }
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      if (pos_ >= text_.size()) {
        throw JsonError("unfinished JSON escape");
      }
      const char escaped = text_[pos_++];
      switch (escaped) {
      case '"':
      case '\\':
      case '/':
        out.push_back(escaped);
        break;
      case 'b':
        out.push_back('\b');
        break;
      case 'f':
        out.push_back('\f');
        break;
      case 'n':
        out.push_back('\n');
        break;
      case 'r':
        out.push_back('\r');
        break;
      case 't':
        out.push_back('\t');
        break;
      case 'u':
        append_unicode_escape(out);
        break;
      default:
        throw JsonError("invalid JSON escape");
      }
    }
    throw JsonError("unfinished JSON string");
  }

  void append_unicode_escape(std::string& out) {
    if (pos_ + 4 > text_.size()) {
      throw JsonError("short unicode escape");
    }
    unsigned code = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = text_[pos_++];
      code <<= 4;
      if (c >= '0' && c <= '9') {
        code += static_cast<unsigned>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        code += static_cast<unsigned>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        code += static_cast<unsigned>(c - 'A' + 10);
      } else {
        throw JsonError("invalid unicode escape");
      }
    }

    if (code <= 0x7F) {
      out.push_back(static_cast<char>(code));
    } else if (code <= 0x7FF) {
      out.push_back(static_cast<char>(0xC0 | (code >> 6)));
      out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xE0 | (code >> 12)));
      out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
  }

  void consume_digits() {
    const std::size_t begin = pos_;
    while (peek() >= '0' && peek() <= '9') {
      ++pos_;
    }
    if (begin == pos_) {
      throw JsonError("expected JSON number digit");
    }
  }

  void skip_ws() {
    while (pos_ < text_.size()) {
      const char c = text_[pos_];
      if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
        break;
      }
      ++pos_;
    }
  }

  char peek() const {
    if (pos_ >= text_.size()) {
      return '\0';
    }
    return text_[pos_];
  }

  void consume(char expected) {
    if (peek() != expected) {
      throw JsonError(std::string("expected '") + expected + "'");
    }
    ++pos_;
  }

  bool try_consume(char expected) {
    if (peek() != expected) {
      return false;
    }
    ++pos_;
    return true;
  }

  void expect(const char* literal) {
    while (*literal) {
      consume(*literal++);
    }
  }

  const std::string& text_;
  std::size_t pos_ = 0;
};

void dump_json(const Json& json, std::ostringstream& out) {
  switch (json.type()) {
  case Json::Type::Null:
    out << "null";
    break;
  case Json::Type::Bool:
    out << (json.as_bool() ? "true" : "false");
    break;
  case Json::Type::Number: {
    const double value = json.as_number();
    if (std::isfinite(value) && std::floor(value) == value) {
      out << static_cast<std::int64_t>(value);
    } else {
      out << std::setprecision(15) << value;
    }
    break;
  }
  case Json::Type::String:
    out << '"' << json_escape(json.as_string()) << '"';
    break;
  case Json::Type::Object: {
    out << '{';
    bool first = true;
    for (const auto& entry : json.as_object()) {
      if (!first) {
        out << ',';
      }
      first = false;
      out << '"' << json_escape(entry.first) << "\":";
      dump_json(entry.second, out);
    }
    out << '}';
    break;
  }
  case Json::Type::Array: {
    out << '[';
    bool first = true;
    for (const auto& item : json.as_array()) {
      if (!first) {
        out << ',';
      }
      first = false;
      dump_json(item, out);
    }
    out << ']';
    break;
  }
  }
}

} // namespace

Json::Json() : type_(Type::Null), bool_value_(false), number_value_(0) {}
Json::Json(std::nullptr_t) : Json() {}
Json::Json(bool value) : type_(Type::Bool), bool_value_(value), number_value_(0) {}
Json::Json(std::int64_t value) : type_(Type::Number), bool_value_(false), number_value_(static_cast<double>(value)) {}
Json::Json(double value) : type_(Type::Number), bool_value_(false), number_value_(value) {}
Json::Json(const char* value) : Json(std::string(value)) {}
Json::Json(std::string value) : type_(Type::String), bool_value_(false), number_value_(0), string_value_(std::move(value)) {}
Json::Json(Object value) : type_(Type::Object), bool_value_(false), number_value_(0), object_value_(std::move(value)) {}
Json::Json(Array value) : type_(Type::Array), bool_value_(false), number_value_(0), array_value_(std::move(value)) {}

Json Json::object() {
  return Json(Object{});
}

Json Json::array() {
  return Json(Array{});
}

Json Json::parse(const std::string& text) {
  return Parser(text).parse();
}

bool Json::as_bool() const {
  if (!is_bool()) {
    throw JsonError("JSON value is not bool");
  }
  return bool_value_;
}

double Json::as_number() const {
  if (!is_number()) {
    throw JsonError("JSON value is not number");
  }
  return number_value_;
}

std::int64_t Json::as_int64() const {
  return static_cast<std::int64_t>(as_number());
}

const std::string& Json::as_string() const {
  if (!is_string()) {
    throw JsonError("JSON value is not string");
  }
  return string_value_;
}

const Json::Object& Json::as_object() const {
  if (!is_object()) {
    throw JsonError("JSON value is not object");
  }
  return object_value_;
}

const Json::Array& Json::as_array() const {
  if (!is_array()) {
    throw JsonError("JSON value is not array");
  }
  return array_value_;
}

Json::Object& Json::as_object() {
  if (!is_object()) {
    throw JsonError("JSON value is not object");
  }
  return object_value_;
}

Json::Array& Json::as_array() {
  if (!is_array()) {
    throw JsonError("JSON value is not array");
  }
  return array_value_;
}

bool Json::contains(const std::string& key) const {
  return is_object() && object_value_.find(key) != object_value_.end();
}

const Json& Json::at(const std::string& key) const {
  const auto& object = as_object();
  const auto it = object.find(key);
  if (it == object.end()) {
    throw JsonError("missing JSON key: " + key);
  }
  return it->second;
}

Json& Json::operator[](const std::string& key) {
  if (is_null()) {
    type_ = Type::Object;
    object_value_.clear();
  }
  return as_object()[key];
}

void Json::push_back(Json value) {
  if (is_null()) {
    type_ = Type::Array;
    array_value_.clear();
  }
  as_array().push_back(std::move(value));
}

std::string Json::dump() const {
  std::ostringstream out;
  dump_json(*this, out);
  return out.str();
}

std::string json_escape(const std::string& value) {
  std::ostringstream out;
  for (const unsigned char c : value) {
    switch (c) {
    case '"':
      out << "\\\"";
      break;
    case '\\':
      out << "\\\\";
      break;
    case '\b':
      out << "\\b";
      break;
    case '\f':
      out << "\\f";
      break;
    case '\n':
      out << "\\n";
      break;
    case '\r':
      out << "\\r";
      break;
    case '\t':
      out << "\\t";
      break;
    default:
      if (c < 0x20) {
        out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(c);
      } else {
        out << static_cast<char>(c);
      }
      break;
    }
  }
  return out.str();
}

} // namespace localsend

