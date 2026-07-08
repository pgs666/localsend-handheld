#pragma once

#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace localsend {

class JsonError : public std::runtime_error {
public:
  explicit JsonError(const std::string& message) : std::runtime_error(message) {}
};

class Json {
public:
  enum class Type { Null, Bool, Number, String, Object, Array };

  using Object = std::map<std::string, Json>;
  using Array = std::vector<Json>;

  Json();
  Json(std::nullptr_t);
  Json(bool value);
  Json(std::int64_t value);
  Json(double value);
  Json(const char* value);
  Json(std::string value);
  Json(Object value);
  Json(Array value);

  static Json object();
  static Json array();
  static Json parse(const std::string& text);

  Type type() const { return type_; }
  bool is_null() const { return type_ == Type::Null; }
  bool is_bool() const { return type_ == Type::Bool; }
  bool is_number() const { return type_ == Type::Number; }
  bool is_string() const { return type_ == Type::String; }
  bool is_object() const { return type_ == Type::Object; }
  bool is_array() const { return type_ == Type::Array; }

  bool as_bool() const;
  double as_number() const;
  std::int64_t as_int64() const;
  const std::string& as_string() const;
  const Object& as_object() const;
  const Array& as_array() const;
  Object& as_object();
  Array& as_array();

  bool contains(const std::string& key) const;
  const Json& at(const std::string& key) const;
  Json& operator[](const std::string& key);
  void push_back(Json value);

  std::string dump() const;

private:
  Type type_;
  bool bool_value_;
  double number_value_;
  std::string string_value_;
  Object object_value_;
  Array array_value_;
};

std::string json_escape(const std::string& value);

} // namespace localsend

