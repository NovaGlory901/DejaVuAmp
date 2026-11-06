#include "fh/json.hpp"

#include <math.h>

#include <charconv>
#include <cmath>
#include <cstdlib>

namespace fh::json {

void Value::set(std::string key, Value v) {
  type_ = Type::Object;
  for (auto& m : obj_) {
    if (m.first == key) {
      m.second = std::move(v);
      return;
    }
  }
  obj_.emplace_back(std::move(key), std::move(v));
}

const Value* Value::find(std::string_view key) const {
  if (type_ != Type::Object) {
    return nullptr;
  }
  for (const auto& m : obj_) {
    if (m.first == key) {
      return &m.second;
    }
  }
  return nullptr;
}

namespace {

struct Reader {
  std::string_view s;
  std::size_t i = 0;
  std::string error;

  bool fail(const char* what) {
    if (error.empty()) {
      error = std::string(what) + " at " + std::to_string(i);
    }
    return false;
  }
  void space() {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) {
      i++;
    }
  }
  bool word(std::string_view w) {
    if (s.substr(i, w.size()) != w) {
      return fail("unexpected token");
    }
    i += w.size();
    return true;
  }

  static void utf8(std::string& out, unsigned c) {
    if (c < 0x80) {
      out += static_cast<char>(c);
    } else if (c < 0x800) {
      out += static_cast<char>(0xC0 | (c >> 6));
      out += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
      out += static_cast<char>(0xE0 | (c >> 12));
      out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
      out += static_cast<char>(0xF0 | (c >> 18));
      out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (c & 0x3F));
    }
  }

  bool hex4(unsigned& out) {
    if (i + 4 > s.size()) {
      return fail("bad \\u escape");
    }
    out = 0;
    for (int k = 0; k < 4; k++) {
      const char c = s[i + static_cast<std::size_t>(k)];
      unsigned d = 0;
      if (c >= '0' && c <= '9') {
        d = static_cast<unsigned>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        d = static_cast<unsigned>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        d = static_cast<unsigned>(c - 'A' + 10);
      } else {
        return fail("bad \\u escape");
      }
      out = out * 16 + d;
    }
    i += 4;
    return true;
  }

  bool str(std::string& out) {
    i++;
    while (true) {
      if (i >= s.size()) {
        return fail("unterminated string");
      }
      const auto c = static_cast<unsigned char>(s[i]);
      if (c == '"') {
        i++;
        return true;
      }
      if (c < 0x20) {
        return fail("control character in string");
      }
      if (c != '\\') {
        out += static_cast<char>(c);
        i++;
        continue;
      }
      i++;
      if (i >= s.size()) {
        return fail("unterminated string");
      }
      const char e = s[i++];
      switch (e) {
        case '"':
          out += '"';
          break;
        case '\\':
          out += '\\';
          break;
        case '/':
          out += '/';
          break;
        case 'b':
          out += '\b';
          break;
        case 'f':
          out += '\f';
          break;
        case 'n':
          out += '\n';
          break;
        case 'r':
          out += '\r';
          break;
        case 't':
          out += '\t';
          break;
        case 'u': {
          unsigned u = 0;
          if (!hex4(u)) {
            return false;
          }
          if (u >= 0xD800 && u <= 0xDBFF) {
            unsigned lo = 0;
            const std::size_t save = i;
            if (s.substr(i, 2) == "\\u") {
              i += 2;
              if (!hex4(lo)) {
                return false;
              }
            }
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
              u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
            } else {
              u = 0xFFFD;
              i = save;
            }
          } else if (u >= 0xDC00 && u <= 0xDFFF) {
            u = 0xFFFD;
          }
          utf8(out, u);
          break;
        }
        default:
          i--;
          return fail("bad escape");
      }
    }
  }

  bool number(double& out) {
    const std::size_t start = i;
    if (i < s.size() && s[i] == '-') {
      i++;
    }
    if (i >= s.size()) {
      return fail("bad number");
    }
    if (s[i] == '0') {
      i++;
    } else if (s[i] >= '1' && s[i] <= '9') {
      while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
        i++;
      }
    } else {
      return fail("bad number");
    }
    if (i < s.size() && s[i] == '.') {
      i++;
      if (i >= s.size() || s[i] < '0' || s[i] > '9') {
        return fail("bad number");
      }
      while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
        i++;
      }
    }
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
      i++;
      if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
        i++;
      }
      if (i >= s.size() || s[i] < '0' || s[i] > '9') {
        return fail("bad number");
      }
      while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
        i++;
      }
    }
    const std::string tmp(s.substr(start, i - start));
    out = std::strtod(tmp.c_str(), nullptr);  // 1e999 gives infinity, like JavaScript
    return true;
  }

  bool value(Value& out, int depth) {
    if (depth > max_depth) {
      return fail("nested too deeply");
    }
    space();
    if (i >= s.size()) {
      return fail("unexpected end");
    }
    const char c = s[i];
    if (c == '{') {
      i++;
      out = Value::object();
      space();
      if (i < s.size() && s[i] == '}') {
        i++;
        return true;
      }
      while (true) {
        space();
        if (i >= s.size() || s[i] != '"') {
          return fail("expected a key");
        }
        std::string key;
        if (!str(key)) {
          return false;
        }
        space();
        if (i >= s.size() || s[i] != ':') {
          return fail("expected ':'");
        }
        i++;
        Value v;
        if (!value(v, depth + 1)) {
          return false;
        }
        out.set(std::move(key), std::move(v));
        space();
        if (i < s.size() && s[i] == ',') {
          i++;
          continue;
        }
        if (i < s.size() && s[i] == '}') {
          i++;
          return true;
        }
        return fail("expected ',' or '}'");
      }
    }
    if (c == '[') {
      i++;
      out = Value::array();
      space();
      if (i < s.size() && s[i] == ']') {
        i++;
        return true;
      }
      while (true) {
        Value v;
        if (!value(v, depth + 1)) {
          return false;
        }
        out.push(std::move(v));
        space();
        if (i < s.size() && s[i] == ',') {
          i++;
          continue;
        }
        if (i < s.size() && s[i] == ']') {
          i++;
          return true;
        }
        return fail("expected ',' or ']'");
      }
    }
    if (c == '"') {
      std::string t;
      if (!str(t)) {
        return false;
      }
      out = Value::string(std::move(t));
      return true;
    }
    if (c == 't') {
      if (!word("true")) {
        return false;
      }
      out = Value::boolean(true);
      return true;
    }
    if (c == 'f') {
      if (!word("false")) {
        return false;
      }
      out = Value::boolean(false);
      return true;
    }
    if (c == 'n') {
      if (!word("null")) {
        return false;
      }
      out = Value::null();
      return true;
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
      double n = NAN;
      if (!number(n)) {
        return false;
      }
      out = Value::number(n);
      return true;
    }
    return fail("unexpected character");
  }
};

void write_string(std::string& out, const std::string& s) {
  out += '"';
  for (const unsigned char c : s) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      default:
        if (c < 0x20) {
          static const char* hex = "0123456789abcdef";
          out += "\\u00";
          out += hex[c >> 4];
          out += hex[c & 15];
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  out += '"';
}

void write_number(std::string& out, double n) {
  if (!std::isfinite(n)) {
    out += "null";
    return;
  }  // as JSON.stringify does
  if (n == 0) {
    out += '0';
    return;
  }
  char buf[40];
  const auto r = std::to_chars(buf, buf + sizeof buf, n);
  out.append(buf, r.ptr);
}

void write(std::string& out, const Value& v, int indent, int level) {
  const auto newline = [&](int lv) {
    if (indent < 0) {
      return;
    }
    out += '\n';
    out.append(static_cast<std::size_t>(indent * lv), ' ');
  };
  switch (v.type()) {
    case Value::Type::Null:
      out += "null";
      break;
    case Value::Type::Bool:
      out += v.as_bool() ? "true" : "false";
      break;
    case Value::Type::Number:
      write_number(out, v.as_number());
      break;
    case Value::Type::String:
      write_string(out, v.as_string());
      break;
    case Value::Type::Array:
      if (v.items().empty()) {
        out += "[]";
        break;
      }
      out += '[';
      for (std::size_t k = 0; k < v.items().size(); k++) {
        if (k) {
          out += ',';
        }
        newline(level + 1);
        write(out, v.items()[k], indent, level + 1);
      }
      newline(level);
      out += ']';
      break;
    case Value::Type::Object:
      if (v.members().empty()) {
        out += "{}";
        break;
      }
      out += '{';
      for (std::size_t k = 0; k < v.members().size(); k++) {
        if (k) {
          out += ',';
        }
        newline(level + 1);
        write_string(out, v.members()[k].first);
        out += indent < 0 ? ":" : ": ";
        write(out, v.members()[k].second, indent, level + 1);
      }
      newline(level);
      out += '}';
      break;
  }
}

}  // namespace

ParseResult parse(std::string_view text) {
  Reader r;
  r.s = text;
  ParseResult out;
  if (!r.value(out.value, 0)) {
    out.error = r.error;
    out.value = {};
    return out;
  }
  r.space();
  if (r.i != text.size()) {
    r.fail("unexpected data after the value");
    out.error = r.error;
    out.value = {};
    return out;
  }
  out.ok = true;
  return out;
}

std::string stringify(const Value& v, int indent) {
  std::string out;
  write(out, v, indent, 0);
  return out;
}

}  // namespace fh::json
