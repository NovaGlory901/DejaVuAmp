// Strict JSON reader and writer (RFC 8259), accepting what JavaScript's JSON.parse accepts. Nesting
// past max_depth is an error (V8 allows deeper); a lone surrogate escape becomes U+FFFD.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fh::json {

class Value {
 public:
  enum class Type { Null, Bool, Number, String, Array, Object };

  Value() = default;
  static Value null() { return {}; }
  static Value boolean(bool b) {
    Value v;
    v.type_ = Type::Bool;
    v.bool_ = b;
    return v;
  }
  static Value number(double n) {
    Value v;
    v.type_ = Type::Number;
    v.num_ = n;
    return v;
  }
  static Value string(std::string s) {
    Value v;
    v.type_ = Type::String;
    v.str_ = std::move(s);
    return v;
  }
  static Value array() {
    Value v;
    v.type_ = Type::Array;
    return v;
  }
  static Value object() {
    Value v;
    v.type_ = Type::Object;
    return v;
  }

  [[nodiscard]] Type type() const { return type_; }
  [[nodiscard]] bool is_null() const { return type_ == Type::Null; }
  [[nodiscard]] bool is_bool() const { return type_ == Type::Bool; }
  [[nodiscard]] bool is_number() const { return type_ == Type::Number; }
  [[nodiscard]] bool is_string() const { return type_ == Type::String; }
  [[nodiscard]] bool is_array() const { return type_ == Type::Array; }
  [[nodiscard]] bool is_object() const { return type_ == Type::Object; }

  [[nodiscard]] bool as_bool() const { return bool_; }
  [[nodiscard]] double as_number() const { return num_; }
  [[nodiscard]] const std::string& as_string() const { return str_; }
  [[nodiscard]] const std::vector<Value>& items() const { return arr_; }
  [[nodiscard]] const std::vector<std::pair<std::string, Value>>& members() const { return obj_; }

  void push(Value v) { arr_.push_back(std::move(v)); }
  void set(
      std::string key,
      Value v);  // adds, or replaces an existing key (a later duplicate wins, as in JSON.parse)
  [[nodiscard]] const Value* find(
      std::string_view key) const;  // nullptr if this is not an object or has no such key

 private:
  Type type_ = Type::Null;
  bool bool_ = false;
  double num_ = 0;
  std::string str_;
  std::vector<Value> arr_;
  std::vector<std::pair<std::string, Value>> obj_;
};

struct ParseResult {
  bool ok = false;
  std::string error;  // e.g. "unexpected character at 12"
  Value value;
};

constexpr int max_depth = 256;

ParseResult parse(std::string_view text);

// indent < 0: compact. indent >= 0: one level per `indent` spaces, like JSON.stringify(v, null,
// indent).
std::string stringify(const Value& v, int indent = -1);

}  // namespace fh::json
