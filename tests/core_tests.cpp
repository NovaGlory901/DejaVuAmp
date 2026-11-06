// Tests for DejaVuAmp's logic (core.hpp): hand-written checks plus parity with
// tests/parity/core.golden.
#include "dva/core.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <string>

#include "check.hpp"
#include "tests.hpp"

using namespace dva;
using fh::json::Value;
using test::check;

static double decode(const Value& v) {
  if (v.is_number()) {
    return v.as_number();
  }
  const std::string& s = v.as_string();
  return s == "NaN" ? NAN : s == "Infinity" ? INFINITY : -INFINITY;
}

static bool same(const Value& a, const Value& b, const std::string& at, std::string& why) {
  if (a.type() != b.type()) {
    why = at + ": different types";
    return false;
  }
  switch (a.type()) {
    case Value::Type::Null:
      return true;
    case Value::Type::Bool:
      if (a.as_bool() != b.as_bool()) {
        why = at + ": different flags";
        return false;
      }
      return true;
    case Value::Type::Number:
      if (std::fabs(a.as_number() - b.as_number()) >
          1e-9 * std::max(1.0, std::fabs(a.as_number()))) {
        why = at + ": " + fh::json::stringify(a) + " vs " + fh::json::stringify(b);
        return false;
      }
      return true;
    case Value::Type::String:
      if (a.as_string() != b.as_string()) {
        why = at + ": \"" + a.as_string() + "\" vs \"" + b.as_string() + "\"";
        return false;
      }
      return true;
    case Value::Type::Array:
      if (a.items().size() != b.items().size()) {
        why = at + ": different lengths";
        return false;
      }
      for (std::size_t i = 0; i < a.items().size(); i++) {
        if (!same(a.items()[i], b.items()[i], at + "[" + std::to_string(i) + "]", why)) {
          return false;
        }
      }
      return true;
    case Value::Type::Object:
      if (a.members().size() != b.members().size()) {
        why = at + ": different keys";
        return false;
      }
      for (const auto& m : a.members()) {
        const Value* o = b.find(m.first);
        if (!o) {
          why = at + ": missing key " + m.first;
          return false;
        }
        if (!same(m.second, *o, at + "." + m.first, why)) {
          return false;
        }
      }
      return true;
  }
  return false;
}

static Value bands_json(const Bands& b) {
  Value a = Value::array();
  for (const double x : b) {
    a.push(Value::number(x));
  }
  return a;
}
static Value preset_json(const EqPreset& p) {
  Value o = Value::object();
  o.set("name", Value::string(p.name));
  o.set("preamp", Value::number(p.preamp));
  o.set("bands", bands_json(p.bands));
  return o;
}

static void hand_tests() {
  check("0 dB is full volume, -60 and below is silence, odd values are silence",
        db_to_gain(0) == 1 && db_to_gain(-60) == 0 && db_to_gain(-100) == 0 &&
            db_to_gain(NAN) == 0 && db_to_gain(INFINITY) == 0 &&
            std::fabs(db_to_gain(-20) - 0.1) < 1e-12);
  check("dB values are written as text, as on the web page",
        format_db(-60) == "muted" && format_db(0.04) == "0.0 dB" &&
            format_db(-12.5) == "-12.5 dB" && signed_db(3) == "+3.0 dB" &&
            signed_db(-0.04) == "-0.0 dB" && signed_db(0) == "0.0 dB");
  check("toFixed rounds halves up like JavaScript (0.25 -> 0.3, 1.15 -> 1.1)",
        signed_db(0.25) == "+0.3 dB" && signed_db(1.15) == "+1.1 dB" &&
            signed_db(-0.25) == "-0.3 dB" && signed_db(9.95) == "+9.9 dB" &&
            signed_db(99.95) == "+100.0 dB");
  check("speeds snap to the nearest choice", snap_speed(1.1) == 1 && snap_speed(1.2) == 1.25 &&
                                                 snap_speed(9) == 2 && snap_speed(0) == 0.5 &&
                                                 snap_speed(0.625) == 0.5);
  check("balance values and their text",
        balance_to_pan(-250) == -1 && balance_to_pan(50) == 0.5 && balance_to_pan(NAN) == 0 &&
            format_balance(0) == "center" && format_balance(-20) == "20% left" &&
            format_balance(35) == "35% right");
  check("time and remaining-time text",
        format_time(205, false) == "03:25" && format_time(205, true) == "-03:25" &&
            format_time(3723, false) == "1:02:03" && format_time(NAN, false) == "--:--" &&
            format_time(-1, true) == "-:--");
  check("the scrolling title text is cleaned and cut",
        ticker_text("Song", "Band") == "Band - Song" && ticker_text("Song", "") == "Song" &&
            ticker_text("", "") == "DejaVuAmp" && ticker_text("a\tb", "") == "a b" &&
            ticker_text(std::string(300, 'x'), "").size() == 200);
  check("19 built-in presets, the first is flat, and the sliders match them by name",
        presets().size() == 19 && presets()[0].name == "Flat" &&
            match_preset(-2, presets()[1].bands) == std::optional<std::string>("Rock") &&
            !match_preset(0.5, Bands{}));
  check("the lowest and highest bands are shelves, the rest bells", [] {
    auto f = filter_specs();
    if (f[0].type != FilterSpec::Type::LowShelf || f[9].type != FilterSpec::Type::HighShelf) {
      return false;
    }
    for (int i = 1; i < 9; i++) {
      if (f[static_cast<std::size_t>(i)].type != FilterSpec::Type::Peaking) {
        return false;
      }
    }
    return f[4].frequency == 1000 && f[4].q == 1.4;
  }());
  {
    const Settings s = sanitize(Value());
    check("settings made from nothing are the defaults",
          s.volume_db == -2 && s.balance == 0 && !s.mono && s.speed == 1 && s.eq_on && s.limiter &&
              s.preset == std::optional<std::string>("Flat") && !s.time_remaining);
    Settings eq_off = s;
    eq_off.eq_on = false;
    eq_off.preamp = 5;
    eq_off.bands[3] = 4;
    check("with the equalizer off its effective curve is flat",
          effective_preamp(eq_off) == 0 && effective_bands(eq_off) == Bands{} &&
              effective_preamp(s) == 0);
  }
  {
    const Settings s =
        sanitize(fh::json::parse(R"({"preamp":3,"bands":[1,2,3,4,5,6,7,8,9,10]})").value);
    auto saved = save_preset(s, "  My <curve>  ");
    check("a curve can be saved under a cleaned name",
          saved.ok && saved.settings.user_presets.size() == 1 &&
              saved.settings.user_presets[0].name == "My curve" &&
              saved.settings.preset == std::optional<std::string>("My curve"));
    check("saving under a built-in name or an empty name is refused",
          save_preset(s, "rock").ok /* a different case of a built-in name is allowed */ &&
              !save_preset(s, "Rock").ok && !save_preset(s, "  ").ok &&
              save_preset(s, "Rock").error.find("built-in") != std::string::npos);
    check("deleting a saved curve removes it",
          delete_preset(saved.settings, "My curve").user_presets.empty());
    Settings full = s;
    for (int i = 0; i < 20; i++) {
      full.user_presets.push_back({"p" + std::to_string(i), 0, Bands{}});
    }
    check("at most 20 curves of your own can be saved",
          !save_preset(full, "one more").ok && save_preset(full, "p3").ok);
  }
  {
    const EqPreset p{"Round trip", 2.5, {1, 2, 3, 4, 5, 6, 7, 8, 9, 10}};
    const PresetFile f = parse_preset_file(export_preset(p));
    check("a preset exported and read back is unchanged", f.ok && f.preset == p, f.error);
    check("a preset file with a wrong marker, size or band count is refused",
          !parse_preset_file(R"({"name":"x"})").ok &&
              !parse_preset_file(std::string(3000, ' ')).ok &&
              !parse_preset_file(R"({"dva-eq":1,"name":"x","bands":[1]})").ok &&
              !parse_preset_file("{").ok);
  }
  {
    // a classic .eqf library: header, then 257-byte name + 10 bands + preamp, each 0..63 (0 = +12
    // dB, 63 = -12 dB)
    const auto entry = [](const std::string& name, std::array<int, 11> v) {
      std::string e = name;
      e.resize(257, '\0');
      for (const int x : v) {
        e += static_cast<char>(x);
      }
      return e;
    };
    const std::string head = std::string("Winamp EQ library file v1.1") + "\x1A!--";
    const std::string file = head + entry("Loud <b>", {0, 0, 63, 63, 31, 32, 21, 42, 0, 63, 0}) +
                             entry("", {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}) +
                             entry("Loud <b>", {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1}) +
                             entry("Caf\xE9", {31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 31});
    EqfFile f = parse_eqf(file);
    check(
        "an .eqf library gives its presets (empty and repeated names skipped, markup "
        "removed, Latin-1 made UTF-8)",
        f.ok && f.presets.size() == 2 && f.presets[0].name == "Loud b" &&
            f.presets[1].name == "Caf\xC3\xA9",
        f.error + " " + std::to_string(f.presets.size()));
    check(
        "in an .eqf entry 0 is +12 dB, 63 is -12 dB, the middle is about 0 and the last byte is "
        "the preamp",
        f.ok && f.presets[0].bands[0] == 12 && f.presets[0].bands[2] == -12 &&
            std::fabs(f.presets[0].bands[4]) <= 0.5 && std::fabs(f.presets[0].bands[5]) <= 0.5 &&
            f.presets[0].preamp == 12 && f.presets[0].bands[6] > 3.5 &&
            f.presets[0].bands[6] < 4.5 && f.presets[0].bands[7] < -3.5 &&
            f.presets[0].bands[7] > -4.5);
    check(
        "an .eqf file ignores a half entry at the end; a wrong header, no entries or a huge file "
        "is refused",
        parse_eqf(file + "junk").ok && parse_eqf(file + "junk").presets.size() == 2 &&
            !parse_eqf("hello").ok && !parse_eqf(head).ok &&
            !parse_eqf(std::string(70000, 'x')).ok && !parse_eqf("").ok);
  }
}

static bool run_case(const Value& c, std::string& why) {
  const std::string kind = c.find("kind")->as_string();
  const Value none;
  const auto field = [&](const char* k) {
    const Value* v = c.find(k);
    return v ? *v : none;
  };
  if (kind == "fmt") {
    const std::string fn = c.find("fn")->as_string();
    const double x = decode(*c.find("in"));
    const Value& want = *c.find("out");
    std::string got;
    double num = 0;
    bool is_num = false;
    if (fn == "dbToGain") {
      num = db_to_gain(x);
      is_num = true;
    } else if (fn == "formatDb") {
      got = format_db(x);
    } else if (fn == "signedDb") {
      got = signed_db(x);
    } else if (fn == "balanceToPan") {
      num = balance_to_pan(x);
      is_num = true;
    } else if (fn == "formatBalance") {
      got = format_balance(static_cast<long long>(x));
    } else if (fn == "snapSpeed") {
      num = snap_speed(x);
      is_num = true;
    } else {
      why = "unknown fn " + fn;
      return false;
    }
    if (is_num) {
      if (std::fabs(num - decode(want)) > 1e-12 * std::max(1.0, std::fabs(num))) {
        why = fn + "(" + fh::json::stringify(*c.find("in")) + ") = " + std::to_string(num) +
              " want " + fh::json::stringify(want);
        return false;
      }
      return true;
    }
    if (got != want.as_string()) {
      why = fn + "(" + fh::json::stringify(*c.find("in")) + ") = \"" + got + "\" want \"" +
            want.as_string() + "\"";
      return false;
    }
    return true;
  }
  if (kind == "time") {
    const std::string got = format_time(decode(*c.find("in")), c.find("remaining")->as_bool());
    if (got != c.find("out")->as_string()) {
      why = "formatTime(" + fh::json::stringify(*c.find("in")) + ") = " + got + " want " +
            c.find("out")->as_string();
      return false;
    }
    return true;
  }
  if (kind == "ticker") {
    const Value* t = c.find("title");
    const Value* a = c.find("artist");
    const std::string got = ticker_text(t && t->is_string() ? t->as_string() : "",
                                        a && a->is_string() ? a->as_string() : "");
    if (got != c.find("out")->as_string()) {
      why = "ticker \"" + got.substr(0, 40) + "\" want \"" +
            c.find("out")->as_string().substr(0, 40) + "\"";
      return false;
    }
    return true;
  }
  if (kind == "sanitize") {
    return same(*c.find("out"), to_json(sanitize(field("raw"))), "sanitize", why);
  }
  if (kind == "file") {
    const PresetFile f = parse_preset_file(c.find("text")->as_string());
    Value got = Value::object();
    got.set("ok", Value::boolean(f.ok));
    if (f.ok) {
      got.set("preset", preset_json(f.preset));
    } else {
      got.set("error", Value::string(f.error));
    }
    return same(*c.find("out"), got, "file", why);
  }
  if (kind == "save") {
    const Value* n = c.find("name");
    const SaveResult r =
        save_preset(sanitize(field("settings")), n && n->is_string() ? n->as_string() : "");
    Value got = Value::object();
    got.set("ok", Value::boolean(r.ok));
    if (r.ok) {
      got.set("settings", to_json(r.settings));
    } else {
      got.set("error", Value::string(r.error));
    }
    return same(*c.find("out"), got, "save", why);
  }
  if (kind == "delete") {
    return same(*c.find("out"),
                to_json(delete_preset(sanitize(field("settings")), c.find("name")->as_string())),
                "delete", why);
  }
  if (kind == "presets") {
    const auto& list = c.find("presets")->items();
    if (list.size() != presets().size()) {
      why = "preset count";
      return false;
    }
    for (std::size_t i = 0; i < list.size(); i++) {
      if (!same(list[i], preset_json(presets()[i]), "preset " + std::to_string(i), why)) {
        return false;
      }
    }
    for (std::size_t i = 0; i < band_count; i++) {
      if (c.find("bands")->items()[i].as_number() != bands_hz[i] ||
          c.find("labels")->items()[i].as_string() != band_labels[i]) {
        why = "band " + std::to_string(i);
        return false;
      }
      const Value& spec = c.find("specs")->items()[i];
      const FilterSpec f = filter_specs()[i];
      const std::string type = f.type == FilterSpec::Type::LowShelf    ? "lowshelf"
                               : f.type == FilterSpec::Type::HighShelf ? "highshelf"
                                                                       : "peaking";
      if (spec.find("type")->as_string() != type ||
          spec.find("frequency")->as_number() != f.frequency ||
          spec.find("Q")->as_number() != f.q) {
        why = "filter spec " + std::to_string(i);
        return false;
      }
    }
    return true;
  }
  why = "unknown kind " + kind;
  return false;
}

static void parity(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    check("parity: open " + path, false);
    return;
  }
  int cases = 0;
  int bad = 0;
  std::string line;
  while (std::getline(in, line)) {
    const auto parsed = fh::json::parse(line);
    if (!parsed.ok) {
      check("parity: golden line " + std::to_string(cases + 1) + " is readable", false,
            parsed.error);
      bad++;
      break;
    }
    cases++;
    std::string why;
    bool ok = false;
    try {
      ok = run_case(parsed.value, why);
    } catch (const std::exception& e) {
      why = e.what();
    }
    if (!ok && ++bad <= 10) {
      check("parity: case " + std::to_string(cases) + " " + parsed.value.find("kind")->as_string(),
            false, why);
    }
  }
  check("parity: " + std::to_string(cases) + " cases, " + std::to_string(bad) +
            " differ from the JavaScript",
        cases > 1000 && bad == 0);
}

void core_tests(const std::string& golden_path) {
  test::section("core");
  hand_tests();
  parity(golden_path);
}
