// dejavuamp: the player window: transport, volume, balance, mono, 10-band equalizer with presets,
// clip guard, spectrum and playlist. Songs come from music/ next to the program (watched) and from
// songs or folders on the command line.
//
//   dejavuamp [--folder DIR] [--music DIR] [--settings FILE] [--device NAME] [--where]
//             [--shot FRAMES FILE.ppm] [--test-song FILE] [SONG-OR-FOLDER ...]
//
// --shot plays the first song silently for that many frames, saves the window as a PPM and quits.
#include <SDL.h>
#include <SDL_opengles2.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "backends/imgui_impl_opengl3.h"
#include "backends/imgui_impl_sdl2.h"
#include "dva/core.hpp"
#include "dva/output.hpp"
#include "dva/player.hpp"
#include "dva/theme.hpp"
#include "fh/analyser.hpp"
#include "fh/audio.hpp"
#include "fh/findroot.hpp"
#include "fh/pathutf8.hpp"
#include "fh/picker.hpp"
#include "fh/userfiles.hpp"
#include "imgui.h"
#include "skin.hpp"

#ifndef DVA_VERSION
#define DVA_VERSION "0"
#endif

namespace fs = std::filesystem;
using dva::Settings;

namespace {

struct Kept {
  Settings audio;
  bool shuffle = false;
  int repeat = 1;  // 0 off, 1 all, 2 one song
  double crossfade = 3;  // seconds the end of one song and the start of the next are blended (0 =
                         // none, the next just follows)
  std::string last;
  std::string theme = "Dreamspace";
};

Kept load_kept(const fs::path& file, std::string& problem) {
  Kept k;
  std::error_code ec;
  if (!fs::exists(file, ec)) {
    k.audio = dva::sanitize(fh::json::Value());
    return k;
  }
  std::ifstream in(file, std::ios::binary);
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (text.size() > 256 * 1024) {
    problem = "settings.json is too big; using the defaults";
    k.audio = dva::sanitize(fh::json::Value());
    return k;
  }
  const fh::json::ParseResult p = fh::json::parse(text);
  if (!p.ok || !p.value.is_object()) {
    problem = "settings.json is not valid; using the defaults";
    k.audio = dva::sanitize(fh::json::Value());
    return k;
  }
  const fh::json::Value* audio = p.value.find("audio");
  k.audio = dva::sanitize(audio ? *audio : fh::json::Value());
  if (const auto* v = p.value.find("shuffle"); v && v->is_bool()) {
    k.shuffle = v->as_bool();
  }
  if (const auto* v = p.value.find("repeat"); v && v->is_number()) {
    k.repeat = static_cast<int>(std::clamp(v->as_number(), 0.0, 2.0));
  }
  if (const auto* v = p.value.find("crossfade");
      v && v->is_number() && std::isfinite(v->as_number())) {
    k.crossfade = std::clamp(std::round(v->as_number() * 2) / 2, 0.0, 12.0);
  }
  if (const auto* v = p.value.find("theme"); v && v->is_string() && v->as_string().size() <= 40) {
    k.theme = v->as_string();
  }
  if (const auto* v = p.value.find("last"); v && v->is_string() && v->as_string().size() < 4096) {
    k.last = v->as_string();
  }
  return k;
}

fh::json::Value kept_json(const Kept& k) {
  fh::json::Value o = fh::json::Value::object();
  o.set("audio", dva::to_json(k.audio));
  o.set("shuffle", fh::json::Value::boolean(k.shuffle));
  o.set("repeat", fh::json::Value::number(k.repeat));
  o.set("crossfade", fh::json::Value::number(k.crossfade));
  o.set("theme", fh::json::Value::string(k.theme));
  o.set("last", fh::json::Value::string(k.last));
  return o;
}

bool save_kept(const fs::path& file, const Kept& k, std::string& error) {
  fs::path tmp = file;
  tmp += ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) {
      error = "could not write " + fh::path_to_utf8(tmp);
      return false;
    }
    out << fh::json::stringify(kept_json(k), 2) << "\n";
    if (!out) {
      error = "could not write " + fh::path_to_utf8(tmp);
      return false;
    }
  }
  std::error_code ec;
  fs::rename(tmp, file, ec);
  if (ec) {
    error = ec.message();
    fs::remove(tmp, ec);
    return false;
  }
  return true;
}

bool same_kept(const Kept& a, const Kept& b) {
  return fh::json::stringify(kept_json(a)) == fh::json::stringify(kept_json(b));
}

struct Playlist {
  struct Dir {
    std::string name, key;
  };  // one folder level above a song: its name, and its path from music/ (the key)
  fs::path root;
  std::vector<fs::path> given;
  std::vector<fs::path> from_folder;
  std::vector<fs::path> all;
  std::vector<std::vector<Dir>>
      dirs;  // for each song in `all`, the folders between music/ and it (none for a song straight
             // in music/ or from the command line)
  std::map<std::string, int> songs_in;
  void rebuild() {
    all = given;
    std::set<fs::path> seen(given.begin(), given.end());
    for (const fs::path& p : from_folder) {
      if (seen.insert(p).second) {
        all.push_back(p);
      }
    }
    dirs.assign(all.size(), {});
    songs_in.clear();
    for (std::size_t i = 0; i < all.size(); i++) {
      const fs::path rel = all[i].lexically_relative(root);
      if (root.empty() || rel.empty() || *rel.begin() == "..") {
        continue;
      }
      std::string key;
      for (auto it = rel.begin(); std::next(it) != rel.end(); ++it) {
        key += (key.empty() ? "" : "/") + fh::path_to_utf8(*it);
        dirs[i].push_back({fh::path_to_utf8(*it), key});
        songs_in[key]++;
      }
    }
  }
  [[nodiscard]] int index_of(const fs::path& p) const {
    for (std::size_t i = 0; i < all.size(); i++) {
      if (all[i] == p) {
        return static_cast<int>(i);
      }
    }
    return -1;
  }
};

void add_path(std::vector<fs::path>& out, const fs::path& p, int depth) {
  std::error_code ec;
  if (fs::is_directory(p, ec)) {
    std::vector<fs::path> kids;
    for (const auto& e : fs::directory_iterator(p, ec)) {
      kids.push_back(e.path());
    }
    std::ranges::sort(kids);
    for (const fs::path& k : kids) {
      if (out.size() >= 5000) {
        return;
      }
      if (fs::is_directory(k, ec)) {
        if (depth < 3) {
          add_path(out, k, depth + 1);
        }
      } else if (dva::looks_like_song(k)) {
        out.push_back(k);
      }
    }
  } else if (fs::is_regular_file(p, ec) && dva::looks_like_song(p)) {
    out.push_back(p);
  }
}

std::string display_name(const fs::path& p) { return fh::path_to_utf8(p.stem()); }

std::string open_folder(const fs::path& folder) {
  std::error_code ec;
  fs::create_directories(folder, ec);
#ifdef _WIN32
  // Windows opens a plain folder path in Explorer (a file:// address would need file:///C:/ and
  // care with the drive colon).
  const std::string win_path = fh::path_to_utf8(fs::absolute(folder, ec).make_preferred());
  if (SDL_OpenURL(win_path.c_str()) != 0)
    std::fprintf(stderr, "could not open the folder %s: %s\n", win_path.c_str(), SDL_GetError());
  return win_path;
#endif
  const std::u8string generic = fs::absolute(folder, ec).generic_u8string();
  const std::string raw(generic.begin(), generic.end());
  std::string url = "file://";
  for (const unsigned char c : raw) {
    if (std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~') {
      url += static_cast<char>(c);
    } else {
      char buf[4];
      std::snprintf(buf, sizeof buf, "%%%02X", c);
      url += buf;
    }
  }
  if (SDL_OpenURL(url.c_str()) != 0) {
    std::fprintf(stderr, "could not open the folder %s: %s\n", raw.c_str(), SDL_GetError());
  }
  return raw;  // shown to the person too: with no file manager installed nothing visible may happen
}

void write_ppm(const std::string& path, const std::vector<std::uint8_t>& rgba, int w, int h) {
  std::ofstream out(fh::path_from_utf8(path), std::ios::binary);
  out << "P6\n" << w << " " << h << "\n255\n";
  for (int y = h - 1; y >= 0; y--) {
    for (int x = 0; x < w; x++) {
      const std::uint8_t* p = &rgba[(static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
                                     static_cast<std::size_t>(x)) *
                                    4];
      out.put(static_cast<char>(p[0])).put(static_cast<char>(p[1])).put(static_cast<char>(p[2]));
    }
  }
}

bool looks_like_eq_file(const fs::path& f) {
  const std::string n = fh::path_to_utf8(f.filename());
  const auto ends_with = [&](const std::string& suffix) {
    if (n.size() <= suffix.size()) {
      return false;
    }
    for (std::size_t i = 0; i < suffix.size(); i++) {
      if (std::tolower(static_cast<unsigned char>(n[n.size() - suffix.size() + i])) != suffix[i]) {
        return false;
      }
    }
    return true;
  };
  return ends_with(".dva-eq.json") || ends_with(".eqf");
}

std::string safe_file_name(const std::string& name) {
  std::string out;
  for (const char c : name) {
    out +=
        (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == ' ') ? c : '_';
  }
  return out.empty() ? "preset" : out;
}

}  // namespace

// Whole-string decimal parse; false for anything else (unlike atoi and sscanf, which accept junk).
static bool parse_int(std::string_view text, int& out) {
  const char* end = text.data() + text.size();
  const auto [ptr, ec] = std::from_chars(text.data(), end, out);
  return ec == std::errc() && ptr == end;
}

static bool parse_size(std::string_view text, int& w, int& h) {
  const auto x = text.find('x');
  return x != std::string_view::npos && parse_int(text.substr(0, x), w) &&
         parse_int(text.substr(x + 1), h);
}

static int run(int argc, char* const* argv) {
#ifdef _WIN32
  // The program is a window program (no console box when started from the Start menu).
  // Started from a terminal, print there (for --where, --version and the messages), unless
  // the output is already going somewhere (a file or a pipe).
  {
    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if ((out == nullptr || out == INVALID_HANDLE_VALUE) && AttachConsole(ATTACH_PARENT_PROCESS)) {
      std::freopen("CONOUT$", "w", stdout);
      std::freopen("CONOUT$", "w", stderr);
    }
  }
#endif
  std::string folder_arg;
  std::string music_arg;
  std::string settings_arg;
  std::string device_hint;
  std::string shot_file;
  int shot_frames = 0;
  int win_w = 1080;
  int win_h = 680;
  bool where = false;
  std::vector<fs::path> args;
  for (int i = 1; i < argc; i++) {
    const std::string a = argv[i];
    if (a == "--folder" && i + 1 < argc) {
      folder_arg = argv[++i];
    } else if (a == "--music" && i + 1 < argc) {
      music_arg = argv[++i];
    } else if (a == "--settings" && i + 1 < argc) {
      settings_arg = argv[++i];
    } else if (a == "--device" && i + 1 < argc) {
      device_hint = argv[++i];
    } else if (a == "--where") {
      where = true;
    } else if (a == "--version") {
      std::printf("dejavuamp %s\n", DVA_VERSION);
      return 0;
    } else if (a == "--size" && i + 1 < argc) {
      if (!parse_size(argv[++i], win_w, win_h)) {
        std::fprintf(stderr, "--size wants WIDTHxHEIGHT\n");
        return 2;
      }
    } else if (a == "--shot" && i + 2 < argc) {
      if (!parse_int(argv[++i], shot_frames)) {
        std::fprintf(stderr, "--shot wants a frame count\n");
        return 2;
      }
      shot_file = argv[++i];
    } else if (a.starts_with("--")) {
      std::fprintf(
          stderr,
          "usage: dejavuamp [--folder DIR] [--music DIR] [--settings FILE] [--device NAME] [--size "
          "WxH] [--where] [--version] [--shot FRAMES FILE.ppm] [SONG-OR-FOLDER ...]\n");
      return 2;
    } else {
      args.emplace_back(fh::path_from_utf8(a));
    }
  }
  const bool headless = shot_frames > 0;
  fs::path root = fh::path_from_utf8(folder_arg);
  if (root.empty()) {  // next to the program (an installed copy), else the nearest parent with a
                       // music/ folder (a build tree in the source folder)
    root = ".";
    if (char* base = SDL_GetBasePath()) {
      root = fh::find_root(fh::path_from_utf8(base), "music");
      SDL_free(base);
    }
  }
  const fs::path music_dir = music_arg.empty() ? root / "music" : fh::path_from_utf8(music_arg);
  const fs::path eq_dir = root / "eq-presets";
  const fs::path settings_file =
      settings_arg.empty() ? root / "settings.json" : fh::path_from_utf8(settings_arg);
  if (where) {
    std::printf("music:         %s\neq-presets:    %s\nsettings:      %s\n",
                fh::path_to_utf8(music_dir).c_str(), fh::path_to_utf8(eq_dir).c_str(),
                fh::path_to_utf8(settings_file).c_str());
    return 0;
  }
  const bool keep_settings = !headless || !settings_arg.empty();

  std::string problem;
  Kept kept =
      keep_settings ? load_kept(settings_file, problem) : Kept{dva::sanitize(fh::json::Value())};
  if (!problem.empty()) {
    std::fprintf(stderr, "%s\n", problem.c_str());
  }
  Kept saved_copy = kept;

  Playlist list;
  for (const fs::path& a : args) {
    add_path(list.given, a, 0);
  }
  fh::FolderWatcher song_watcher(
      music_dir, [](const fs::path& f) { return dva::looks_like_song(f); }, true);
  fh::FolderWatcher eq_watcher(eq_dir, looks_like_eq_file);
  std::vector<dva::EqPreset> file_presets;
  const auto rescan_songs = [&] {
    list.root = music_dir;
    list.from_folder =
        fh::list_files(music_dir, [](const fs::path& f) { return dva::looks_like_song(f); }, true);
    list.rebuild();
  };
  const auto rescan_eq = [&] {
    file_presets.clear();
    for (const fs::path& f : fh::list_files(eq_dir, looks_like_eq_file)) {
      std::ifstream in(f, std::ios::binary);
      const std::string text((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
      std::vector<dva::EqPreset> found;
      if (f.extension() == ".json") {
        const dva::PresetFile pf = dva::parse_preset_file(text);
        if (!pf.ok) {
          std::fprintf(stderr, "equalizer preset %s: %s\n", fh::path_to_utf8(f.filename()).c_str(),
                       pf.error.c_str());
          continue;
        }
        found.push_back(pf.preset);
      } else {  // an .eqf library: many presets in one file
        const dva::EqfFile ef = dva::parse_eqf(text);
        if (!ef.ok) {
          std::fprintf(stderr, "equalizer library %s: %s\n", fh::path_to_utf8(f.filename()).c_str(),
                       ef.error.c_str());
          continue;
        }
        found = ef.presets;
      }
      for (const dva::EqPreset& p : found) {
        if (dva::preset_by_name(p.name)) {
          continue;
        }
        bool dup = false;
        for (const auto& q : file_presets) {
          if (q.name == p.name) {
            dup = true;
          }
        }
        if (!dup && file_presets.size() < 100) {
          file_presets.push_back(p);
        }
      }
    }
  };
  song_watcher.poll();
  eq_watcher.poll();
  rescan_songs();
  rescan_eq();

#ifdef _WIN32
  // The program links ANGLE's libGLESv2, so the context must come from ANGLE's EGL too (not a
  // native WGL "ES profile" context).
  SDL_SetHint(SDL_HINT_OPENGL_ES_DRIVER, "1");
#endif
  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    std::fprintf(stderr, "SDL could not start: %s\n", SDL_GetError());
    return 1;
  }
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
  SDL_Window* window =
      SDL_CreateWindow("DejaVuAmp", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, win_w, win_h,
                       SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
  if (!window) {
    std::fprintf(stderr, "could not open a window: %s\n", SDL_GetError());
    return 1;
  }
  SDL_GLContext gl = SDL_GL_CreateContext(window);
  if (!gl) {
    std::fprintf(stderr, "could not get an OpenGL ES 2 context: %s\n", SDL_GetError());
    return 1;
  }
  SDL_GL_SetSwapInterval(1);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.LogFilename = nullptr;
  SDL_SetWindowMinimumSize(window, 520, 420);
  fs::path asset_root = ".";  // the font lives with the program (or the source, in a build tree),
                              // not with the person's folders
  if (char* base = SDL_GetBasePath()) {
    asset_root = fh::find_root(fh::path_from_utf8(base), "assets");
    SDL_free(base);
  }
  const bool font_ok = skin::load_fonts(asset_root, 22.0F);
  if (!font_ok) {
    std::fprintf(stderr,
                 "the VT323 font (assets/fonts/VT323-Regular.ttf) was not found next to the "
                 "program; using a plain font\n");
  }

  const fs::path theme_dir = root / "themes";
  auto looks_like_theme = [](const fs::path& f) {
    const std::string n = fh::path_to_utf8(f.filename());
    const std::string suf = ".dva-theme.json";
    return n.size() > suf.size() && n.ends_with(suf);
  };
  fh::FolderWatcher theme_watcher(theme_dir, looks_like_theme);
  std::vector<dva::Theme> themes;
  skin::Colors colors = skin::make_colors(dva::default_theme());
  const auto rescan_themes = [&] {
    themes = dva::builtin_themes();
    for (const fs::path& f : fh::list_files(theme_dir, looks_like_theme)) {
      std::ifstream in(f, std::ios::binary);
      const std::string text((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
      dva::ThemeFile tf = dva::parse_theme_file(text);
      if (!tf.ok) {
        std::fprintf(stderr, "theme %s: %s\n", fh::path_to_utf8(f.filename()).c_str(),
                     tf.error.c_str());
        continue;
      }
      if (std::ranges::any_of(themes,
                              [&](const dva::Theme& t) { return t.name == tf.theme.name; }) ||
          themes.size() >= 40) {
        continue;
      }
      themes.push_back(tf.theme);
    }
    const dva::Theme* pick = &themes.front();
    for (const dva::Theme& t : themes) {
      if (t.name == kept.theme) {
        pick = &t;
      }
    }
    colors = skin::make_colors(*pick);
    skin::apply_style(colors);
  };
  theme_watcher.poll();
  ImGui_ImplSDL2_InitForOpenGL(window, gl);
  ImGui_ImplOpenGL3_Init("#version 100");
  rescan_themes();

  dva::Player player(48000);
  player.set_settings(kept.audio);
  player.set_crossfade(kept.crossfade);
  dva::AudioOutput output;
  std::string audio_error;
  bool have_sound = false;
  if (!headless) {
    have_sound = output.start(player, device_hint, audio_error);
    if (!have_sound) {
      std::fprintf(stderr, "no sound: %s\n", audio_error.c_str());
    }
  }

  fh::Analyser analyser(2048, 0.7);
  fh::AudioFeatures features;
  std::vector<std::uint8_t> spectrum;
  std::vector<std::uint8_t> waveform;
  std::vector<float> tap_buf(2048);
  fh::PresetPicker picker;
  int current = -1;
  std::string notice;
  char name_buf[48] = "";
  std::string say_text;
  bool say_bad = false;
  int vis_mode = 0;  // the sound display: 0 bars, 1 scope, 2 off
  double notice_until = 0;
  bool running = true;
  long frames = 0;
  const Uint64 freq = SDL_GetPerformanceFrequency();
  const Uint64 t0 = SDL_GetPerformanceCounter();
  auto now_ms = [&] {
    return static_cast<double>(SDL_GetPerformanceCounter() - t0) * 1000.0 /
           static_cast<double>(freq);
  };
  auto say = [&](const std::string& t) {
    notice = t;
    notice_until = now_ms() + 6000;
  };
  double changed_at = -1;
  double last_scan = 0;

  // The song that should follow the playing one: it is lined up in the player as soon as a song
  // starts, so the blend (or the gapless hand-over) is ready.
  std::mt19937 gen{std::random_device{}()};
  std::deque<int> recent;  // the last few songs played, so shuffle does not repeat itself
  int queued_index = -1;
  std::set<std::string> open_dirs;
  int revealed = -2;
  bool scroll_to_current = false;
  unsigned seen_serial = 0;
  auto choose_next = [&]() -> int {
    const int n = static_cast<int>(list.all.size());
    if (n == 0 || current < 0) {
      return -1;
    }
    if (kept.repeat == 2) {
      return current;
    }
    if (kept.shuffle) {
      if (n == 1) {
        return kept.repeat == 1 ? 0 : -1;
      }
      for (int tries = 0; tries < 30; tries++) {
        const int i = static_cast<int>(gen() % static_cast<unsigned>(n));
        if (i != current && (tries >= 20 || std::ranges::find(recent, i) == recent.end())) {
          return i;
        }
      }
      return (current + 1) % n;
    }
    if (current + 1 < n) {
      return current + 1;
    }
    return kept.repeat == 1 ? 0 : -1;
  };
  auto requeue = [&] {
    player.clear_next();
    queued_index = -1;
    const int i = choose_next();
    if (i < 0) {
      return;
    }
    std::string err;
    if (player.queue_next(list.all[static_cast<std::size_t>(i)], err)) {
      queued_index = i;
    } else {
      say(display_name(list.all[static_cast<std::size_t>(i)]) + ": " + err);
    }
  };
  auto note_current = [&](int index) {
    current = index;
    recent.push_back(index);
    while (recent.size() > 4) {
      recent.pop_front();
    }
    kept.last = fh::path_to_utf8(list.all[static_cast<std::size_t>(index)]);
  };
  auto play = [&](int index) {
    if (index < 0 || std::cmp_greater_equal(index, list.all.size())) {
      return false;
    }
    std::string err;
    if (!player.skip_to(list.all[static_cast<std::size_t>(index)], err)) {
      say(display_name(list.all[static_cast<std::size_t>(index)]) + ": " + err);
      return false;
    }
    note_current(index);
    picker.show(fh::path_to_utf8(list.all[static_cast<std::size_t>(index)]));
    seen_serial = player.status().track_serial;
    analyser.reset();
    requeue();
    return true;
  };
  auto names = [&] {
    std::vector<std::string> v;
    v.reserve(list.all.size());
    for (const auto& p : list.all) {
      v.push_back(fh::path_to_utf8(p));
    }
    return v;
  };
  const auto step = [&](int dir) {
    if (list.all.empty()) {
      return;
    }
    if (dir > 0 && queued_index >= 0) {
      play(queued_index);
      return;
    }
    if (kept.shuffle) {
      std::optional<std::string> n = dir > 0 ? picker.next(names()) : picker.prev();
      if (n) {
        const int i = list.index_of(fh::path_from_utf8(*n));
        if (i >= 0) {
          play(i);
        }
      }
      return;
    }
    const int n = static_cast<int>(list.all.size());
    play(((current < 0 ? (dir > 0 ? -1 : 0) : current) + dir + n) % n);
  };
  const auto change =
      [&](const Settings& next) {  // goes through sanitize so every setting stays valid
        kept.audio = dva::sanitize(dva::to_json(next));
        player.set_settings(kept.audio);
      };

  // start with the song that was open last time, if it is still there (it does not start playing by
  // itself: the person decides)
  {
    int at = list.index_of(fh::path_from_utf8(kept.last));
    const bool asked = !list.given.empty();  // a song named on the command line plays at once
    if ((headless || asked) && !list.all.empty()) {
      at = 0;
    }
    if (at >= 0) {
      std::string err;
      if (player.load(list.all[static_cast<std::size_t>(at)], err, headless || asked)) {
        note_current(at);
        picker.show(fh::path_to_utf8(list.all[static_cast<std::size_t>(at)]));
        seen_serial = player.status().track_serial;
        player.set_crossfade(kept.crossfade);
        requeue();
      }
    }
  }

  while (running) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      ImGui_ImplSDL2_ProcessEvent(&e);
      if (e.type == SDL_QUIT) {
        running = false;
      } else if (e.type == SDL_KEYDOWN && !io.WantTextInput) {
        const dva::Player::Status st = player.status();
        switch (e.key.keysym.sym) {
          case SDLK_SPACE:
            if (st.loaded) {
              player.toggle();
            } else if (!list.all.empty()) {
              play(current >= 0 ? current : 0);
            }
            break;
          case SDLK_RIGHT:
            player.seek(st.position + 5);
            break;
          case SDLK_LEFT:
            player.seek(st.position - 5);
            break;
          case SDLK_UP: {
            Settings s = kept.audio;
            s.volume_db = std::min(0.0, s.volume_db + 1);
            change(s);
            break;
          }
          case SDLK_DOWN: {
            Settings s = kept.audio;
            s.volume_db = std::max(-60.0, s.volume_db - 1);
            change(s);
            break;
          }
          case SDLK_n:
            step(+1);
            break;
          case SDLK_p:
            step(-1);
            break;
          case SDLK_m: {
            Settings s = kept.audio;
            s.mono = !s.mono;
            change(s);
            break;
          }
          case SDLK_e: {
            Settings s = kept.audio;
            s.eq_on = !s.eq_on;
            change(s);
            break;
          }
          case SDLK_l: {
            Settings s = kept.audio;
            s.limiter = !s.limiter;
            change(s);
            break;
          }
          case SDLK_s:
            kept.shuffle = !kept.shuffle;
            break;
          case SDLK_q:
          case SDLK_ESCAPE:
            running = false;
            break;
          default:
            break;
        }
      }
    }
    const double now = now_ms();
    {
      // the next song has started coming out of the speakers (a blend, or a gapless hand-over):
      // follow along and line up the one after it
      const dva::Player::Status ps = player.status();
      if (ps.track_serial != seen_serial) {
        seen_serial = ps.track_serial;
        const int idx = list.index_of(ps.path);
        if (idx >= 0) {
          note_current(idx);
          picker.show(ps.path);
          requeue();
        }
      }
    }
    if (player.take_ended()) {  // nothing was lined up (the end of the list with repeat off, or a
                                // song that would not open)
      if (kept.repeat == 2) {
        play(current);
      } else if (kept.repeat == 1 && !list.all.empty()) {
        step(+1);
      }
    }
    if (!headless && now - last_scan > 2000) {
      last_scan = now;
      if (song_watcher.poll().any()) {
        const fs::path cur =
            current >= 0 ? list.all[static_cast<std::size_t>(current)] : fs::path();
        rescan_songs();
        current = cur.empty() ? -1 : list.index_of(cur);
        requeue();
      }
      if (eq_watcher.poll().any()) {
        rescan_eq();
      }
      if (theme_watcher.poll().any()) {
        rescan_themes();
      }
    }

    player.latest(tap_buf.data(), tap_buf.size());
    analyser.push(tap_buf);
    analyser.read(spectrum, waveform);
    features.update(spectrum, waveform, player.sample_rate(), now);

    const dva::Player::Status st = player.status();
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
    const ImVec2 display = io.DisplaySize;
    skin::backdrop(colors, display);
    Settings s = kept.audio;
    bool touched = false;
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(display);
    ImGui::Begin(
        "##amp", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBackground |
            ImGuiWindowFlags_NoBringToFrontOnFocus |
            (display.x >= 1040 ? (ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)
                               : 0));
    {
      const float margin = 16;
      const float gap = 14;
      const float main_w = 460;
      const float main_h = 306;
      const float eq_h = 300;
      const float opt_h = 132;
      const bool wide = display.x >= 1040;
      const float right_w =
          wide ? display.x - 2 * margin - gap - main_w : std::min(display.x - 2 * margin, 640.0F);
      const float left_w = wide ? main_w : std::min(display.x - 2 * margin, 640.0F);
      const float list_h = wide ? std::max(160.0F, display.y - 2 * margin - main_h - gap) : 300.0F;
      const ImVec2 main_at(margin, margin);
      ImVec2 eq_at;
      ImVec2 opt_at;
      ImVec2 list_at;
      if (wide) {
        eq_at = ImVec2(margin + main_w + gap, margin);
        opt_at = ImVec2(eq_at.x, margin + eq_h + gap);
        list_at = ImVec2(margin, margin + main_h + gap);
      } else {
        eq_at = ImVec2(margin, margin + main_h + gap);
        opt_at = ImVec2(margin, eq_at.y + eq_h + gap);
        list_at = ImVec2(margin, opt_at.y + opt_h + gap);
      }
      const float eq_w = wide ? right_w : left_w;
      const float opt_w = eq_w;
      const float list_w = wide ? main_w : left_w;
      const float content_bottom =
          (wide ? std::max(list_at.y + list_h, opt_at.y + opt_h) : list_at.y + list_h) + margin;
      ImGui::SetCursorPos(
          ImVec2(0, content_bottom));  // makes the window scroll when everything does not fit
      ImGui::Dummy(ImVec2(1, 1));
      const ImVec2 origin = ImGui::GetWindowPos();
      const float scroll_y = ImGui::GetScrollY();
      (void)origin;
      (void)scroll_y;

      ImGui::SetCursorPos(main_at);
      skin::panel_begin("main", ImVec2(left_w, main_h), "DejaVuAmp", colors);
      {
        const float w = left_w;
        const ImVec2 base = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        auto at = [&](float x, float y) { return ImVec2(base.x + x, base.y + y); };
        skin::inset_box(at(10, 34), at(w - 10, 152), colors);
        const std::string time_text = dva::format_time(
            kept.audio.time_remaining && st.duration >= 0 ? st.duration - st.position : st.position,
            kept.audio.time_remaining && st.duration >= 0);
        skin::glow_text(at(18, 40), colors.cyan, time_text.c_str(), 54, colors);
        ImGui::SetCursorScreenPos(at(14, 38));
        if (ImGui::InvisibleButton("##time", ImVec2(150, 54))) {
          s.time_remaining = !s.time_remaining;
          touched = true;
        }
        if (ImGui::IsItemHovered()) {
          ImGui::SetTooltip("Click for elapsed or remaining time");
        }
        const ImVec2 vis_a = at(170, 38);
        const ImVec2 vis_b = at(w - 14, 38 + 54);
        {
          dl->AddRectFilled(vis_a, vis_b, colors.deep);
          static std::vector<float> peaks(24, 0.0F);
          const float vw = vis_b.x - vis_a.x;
          const float vh = vis_b.y - vis_a.y;
          if (vis_mode == 0) {
            const double bin_hz = player.sample_rate() / 2 /
                                  static_cast<double>(spectrum.size() ? spectrum.size() : 1);
            const double lo = 40;
            const double hi = 16000;
            const float bw = vw / 24.0F;
            for (int i = 0; i < 24; i++) {
              const double f0 = lo * std::pow(hi / lo, i / 24.0);
              const double f1 = lo * std::pow(hi / lo, (i + 1) / 24.0);
              const int a0 = static_cast<int>(std::floor(f0 / bin_hz));
              const int a1 = std::max(a0 + 1, static_cast<int>(std::ceil(f1 / bin_hz)));
              double sum = 0;
              int n = 0;
              for (int k = a0; k < a1 && std::cmp_less(k, spectrum.size()); k++, n++) {
                sum += spectrum[static_cast<std::size_t>(k)];
              }
              const float v = n ? static_cast<float>(sum / n / 255.0) : 0.0F;
              const float h = std::round(v * (vh - 2));
              peaks[static_cast<std::size_t>(i)] =
                  std::max(h, peaks[static_cast<std::size_t>(i)] - 0.5F);
              const float x0 = vis_a.x + static_cast<float>(i) * bw + 1;
              const float x1 = vis_a.x + static_cast<float>(i + 1) * bw - 1;
              const ImU32 top =
                  skin::mix(colors.cyan, colors.pink,
                            h / std::max(1.0F, vh));  // the gradient runs over the whole display,
                                                      // so tall bars end pinker
              if (h > 0) {
                dl->AddRectFilledMultiColor(ImVec2(x0, vis_b.y - h), ImVec2(x1, vis_b.y), top, top,
                                            colors.cyan, colors.cyan);
              }
              dl->AddRectFilled(ImVec2(x0, vis_b.y - peaks[static_cast<std::size_t>(i)] - 2),
                                ImVec2(x1, vis_b.y - peaks[static_cast<std::size_t>(i)]),
                                colors.pink);
            }
          } else if (vis_mode == 1 && !waveform.empty()) {
            ImVec2 prev(vis_a.x, vis_a.y + vh * 0.5F);
            for (float x = 0; x < vw; x += 1.0F) {
              const float y = vis_a.y + static_cast<float>(waveform[static_cast<std::size_t>(
                                            x / vw * static_cast<float>(waveform.size() - 1))]) /
                                            255.0F * vh;
              const ImVec2 cur(vis_a.x + x, y);
              if (x > 0) {
                dl->AddLine(prev, cur, colors.cyan, 1.5F);
              }
              prev = cur;
            }
          } else {
            dl->AddRectFilled(ImVec2(vis_a.x, vis_b.y - 2), vis_b, colors.border);
          }
          ImGui::SetCursorScreenPos(vis_a);
          if (ImGui::InvisibleButton("##vis", ImVec2(vw, vh))) {
            vis_mode = (vis_mode + 1) % 3;
          }
          if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Click to change the display");
          }
        }
        // the title: scrolls back and forth when it does not fit, like the web version
        {
          const std::string title = dva::ticker_text(
              st.tags.title.empty() ? (st.path.empty() ? "" : display_name(st.path))
                                    : st.tags.title,
              st.tags.artist);
          const float tx0 = base.x + 16;
          const float tx1 = base.x + w - 16;
          const float ty = base.y + 100;
          for (float x = tx0; x < tx1; x += 6) {
            dl->AddLine(ImVec2(x, ty - 3), ImVec2(x + 3, ty - 3), colors.border);
          }
          const float tw = ImGui::CalcTextSize(title.c_str()).x;
          const float over = tw - (tx1 - tx0) + 12;
          float shift = 0;
          if (over > 0) {
            const double u = std::fmod(now / 14000.0, 2.0);
            const double tri = u <= 1 ? u : 2 - u;
            shift = -over * static_cast<float>(std::clamp((tri - 0.25) / 0.75, 0.0, 1.0));
          }
          dl->PushClipRect(ImVec2(tx0, ty - 2), ImVec2(tx1, ty + 24), true);
          dl->AddText(ImVec2(tx0 + shift, ty), colors.fg, title.c_str());
          dl->PopClipRect();
          const std::string info =
              (st.format.empty() ? std::string() : st.format + " \xC2\xB7 ") +
              (s.mono ? "mono" : "stereo") + " \xC2\xB7 " +
              (s.speed == 1 ? "1"
                            : std::to_string(s.speed).substr(
                                  0, s.speed == 0.75 || s.speed == 1.25 ? 4 : 3)) +
              "x" + (st.underruns ? " \xC2\xB7 sound broke up" : "");
          skin::text(ImVec2(base.x + 16, base.y + 124), colors.dim, info.c_str(), 18);
        }
        {
          auto pos = static_cast<float>(st.position);
          const float len = static_cast<float>(std::max(st.duration, 0.0));
          ImGui::SetCursorScreenPos(at(10, 162));
          ImGui::SetNextItemWidth(w - 20);
          ImGui::BeginDisabled(!(st.loaded && st.duration > 0));
          if (ImGui::SliderFloat("##seek", &pos, 0, std::max(len, 0.001F), "")) {
            player.seek(pos);
          }
          ImGui::EndDisabled();
        }
        ImGui::SetCursorScreenPos(at(10, 192));
        if (skin::icon_button("prev", skin::Icon::Prev, ImVec2(44, 30), colors)) {
          step(-1);
        }
        ImGui::SameLine(0, 4);
        if (skin::icon_button("play", st.playing ? skin::Icon::Pause : skin::Icon::Play,
                              ImVec2(44, 30), colors)) {
          if (st.loaded) {
            player.toggle();
          } else if (!list.all.empty()) {
            play(current >= 0 ? current : 0);
          }
        }
        ImGui::SameLine(0, 4);
        if (skin::icon_button("next", skin::Icon::Next, ImVec2(44, 30), colors)) {
          step(+1);
        }
        ImGui::SetCursorScreenPos(at(172, 196));
        if (skin::toggle("MONO", s.mono, colors)) {
          s.mono = !s.mono;
          touched = true;
        }
        ImGui::SameLine(0, 6);
        if (skin::toggle("EQ", s.eq_on, colors)) {
          s.eq_on = !s.eq_on;
          touched = true;
        }
        ImGui::SameLine(0, 6);
        if (skin::toggle("LIMIT", s.limiter, colors)) {
          s.limiter = !s.limiter;
          touched = true;
        }
        if (ImGui::IsItemHovered()) {
          ImGui::SetTooltip("Clip guard: nothing gets above -2 dB%s",
                            st.gain_reduction_db > 0.2 ? " (working now)" : "");
        }
        ImGui::SameLine(0, 6);
        {
          const std::string sp =
              (s.speed == 1 ? std::string("1")
                            : std::to_string(s.speed).substr(
                                  0, s.speed == 0.75 || s.speed == 1.25 ? 4 : 3)) +
              "x";
          ImGui::SetNextItemWidth(70);
          if (ImGui::BeginCombo("##speed", sp.c_str())) {
            for (const double v : dva::speeds) {
              const std::string l =
                  (v == 1 ? std::string("1")
                          : std::to_string(v).substr(0, v == 0.75 || v == 1.25 ? 4 : 3)) +
                  "x";
              if (ImGui::Selectable(l.c_str(), v == s.speed)) {
                s.speed = v;
                touched = true;
              }
            }
            ImGui::EndCombo();
          }
        }
        const auto labelled = [&](const char* name, float y) {
          skin::text(at(10, y + 1), colors.dim, name, 0);
          ImGui::SetCursorScreenPos(at(48, y));
          ImGui::SetNextItemWidth(w - 48 - 90);
        };
        labelled("vol", 238);
        auto vdb = static_cast<float>(s.volume_db);
        if (ImGui::SliderFloat("##vol", &vdb, -60, 0, "")) {
          s.volume_db = std::round(vdb * 2) / 2;
          touched = true;
        }
        skin::text(at(w - 84, 239), colors.fg, dva::format_db(s.volume_db).c_str(), 18);
        labelled("bal", 268);
        int bal = s.balance;
        if (ImGui::SliderInt("##bal", &bal, -100, 100, "")) {
          s.balance = bal;
          touched = true;
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
          s.balance = 0;
          touched = true;
        }
        skin::text(at(w - 84, 269), colors.fg, dva::format_balance(s.balance).c_str(), 18);
      }
      skin::panel_end();

      ImGui::SetCursorPos(eq_at);
      skin::panel_begin("eq", ImVec2(eq_w, eq_h), "Equalizer", colors);
      {
        const float w = eq_w;
        const ImVec2 base = ImGui::GetCursorScreenPos();
        const ImDrawList* dl = ImGui::GetWindowDrawList();
        const auto at = [&](float x, float y) { return ImVec2(base.x + x, base.y + y); };
        ImGui::SetCursorScreenPos(at(10, 34));
        ImGui::SetNextItemWidth(210);
        const std::string shown = s.preset ? *s.preset : "- custom -";
        if (ImGui::BeginCombo("##preset", shown.c_str())) {
          const auto item = [&](const dva::EqPreset& p) {
            if (ImGui::Selectable(p.name.c_str(), s.preset && *s.preset == p.name)) {
              s.preamp = p.preamp;
              s.bands = p.bands;
              touched = true;
            }
          };
          for (const auto& p : dva::presets()) {
            item(p);
          }
          if (!s.user_presets.empty()) {
            ImGui::SeparatorText("Yours");
            for (const auto& p : s.user_presets) {
              item(p);
            }
          }
          if (!file_presets.empty()) {
            ImGui::SeparatorText("From files");
            for (const auto& p : file_presets) {
              item(p);
            }
          }
          ImGui::EndCombo();
        }
        ImGui::SameLine(0, 8);
        if (skin::button("Reset", ImVec2(0, 0), colors)) {
          s.preamp = 0;
          s.bands = dva::Bands{};
          touched = true;
          say_text = "Flat.";
          say_bad = false;
        }
        ImGui::SameLine(0, 6);
        if (skin::button("Export", ImVec2(0, 0), colors)) {
          const dva::EqPreset p{s.preset ? *s.preset : "My preset", s.preamp, s.bands};
          std::error_code ec;
          fs::create_directories(eq_dir, ec);
          std::ofstream out(eq_dir / (safe_file_name(p.name) + ".dva-eq.json"), std::ios::binary);
          out << dva::export_preset(p) << "\n";
          say_text = "Saved " + safe_file_name(p.name) + ".dva-eq.json in eq-presets/";
          say_bad = false;
          rescan_eq();
        }
        const bool can_delete = s.preset && !dva::preset_by_name(*s.preset) &&
                                std::ranges::any_of(s.user_presets, [&](const dva::EqPreset& p) {
                                  return p.name == *s.preset;
                                });
        if (can_delete) {
          ImGui::SameLine(0, 6);
          if (skin::button("Delete", ImVec2(0, 0), colors)) {
            s = dva::delete_preset(s, *s.preset);
            touched = true;
            say_text = "Deleted.";
            say_bad = false;
          }
        }
        ImGui::SetCursorScreenPos(at(10, 66));
        ImGui::SetNextItemWidth(210);
        ImGui::InputTextWithHint("##name", "name this curve", name_buf, sizeof name_buf);
        ImGui::SameLine(0, 8);
        if (skin::button("Save", ImVec2(0, 0), colors)) {
          const dva::SaveResult r = dva::save_preset(s, name_buf);
          if (r.ok) {
            s = r.settings;
            touched = true;
            name_buf[0] = 0;
            say_text = "Saved.";
            say_bad = false;
          } else {
            say_text = r.error;
            say_bad = true;
          }
        }
        skin::text(at(10, 98), say_bad ? colors.bad : colors.cyan, say_text.c_str(), 18);

        const float box_y = 122;
        const float box_h = 168;
        skin::inset_box(at(10, box_y), at(w - 10, box_y + box_h), colors);
        const float alpha = s.eq_on ? 1.0F : 0.45F;
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
        const float inner = w - 20 - 16;
        const float scale_w = 34;
        const float col_w = std::clamp((inner - scale_w) / 11.0F, 30.0F, 60.0F);
        const auto column = [&](float x, const char* label, double* value, const char* id) {
          const float cx = base.x + x + col_w * 0.5F;
          char db[16];
          std::snprintf(db, sizeof db, "%s%g", *value > 0 ? "+" : "", *value);
          const float tw = ImGui::CalcTextSize(db).x * (18.0F / ImGui::GetFontSize());
          skin::text(ImVec2(cx - tw * 0.5F, base.y + box_y + 4), colors.cyan, db, 18);
          ImGui::SetCursorScreenPos(ImVec2(cx - 11, base.y + box_y + 22));
          auto v = static_cast<float>(*value);
          if (ImGui::VSliderFloat(id, ImVec2(22, 116), &v, -12.0F, 12.0F, "")) {
            *value = std::round(v * 2) / 2;
            touched = true;
          }
          if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
            *value = 0;
            touched = true;
          }
          const float lw = ImGui::CalcTextSize(label).x * (18.0F / ImGui::GetFontSize());
          skin::text(ImVec2(cx - lw * 0.5F, base.y + box_y + 22 + 116 + 1), colors.dim, label, 18);
        };
        float x = 18;
        column(x, "PRE", &s.preamp, "##pre");
        x += col_w;
        for (const char* t : {"+12", "0", "-12"}) {
          const float yy = std::strcmp(t, "+12") == 0 ? 22
                           : std::strcmp(t, "0") == 0 ? 22 + 58 - 7
                                                      : 22 + 116 - 14;
          skin::text(ImVec2(base.x + x + 2, base.y + box_y + yy), colors.dim, t, 15);
        }
        x += scale_w;
        for (int i = 0; i < dva::band_count; i++, x += col_w) {
          column(x, dva::band_labels[static_cast<std::size_t>(i)],
                 &s.bands[static_cast<std::size_t>(i)],
                 (std::string("##b") + std::to_string(i)).c_str());
        }
        ImGui::PopStyleVar();
        (void)dl;
      }
      skin::panel_end();

      ImGui::SetCursorPos(opt_at);
      skin::panel_begin("options", ImVec2(opt_w, opt_h), "Options", colors);
      {
        const ImVec2 base = ImGui::GetCursorScreenPos();
        const auto at = [&](float x, float y) { return ImVec2(base.x + x, base.y + y); };
        ImGui::SetCursorScreenPos(at(10, 36));
        if (skin::toggle("SHUFFLE", kept.shuffle, colors)) {
          kept.shuffle = !kept.shuffle;
          requeue();
        }
        ImGui::SameLine(0, 6);
        const char* repeats[] = {"REPEAT OFF", "REPEAT ALL", "REPEAT ONE"};
        if (skin::toggle(repeats[kept.repeat], kept.repeat != 0, colors)) {
          kept.repeat = (kept.repeat + 1) % 3;
          requeue();
        }
        ImGui::SameLine(0, 14);
        if (skin::button("Music folder", ImVec2(0, 0), colors)) {
          say("Music folder: " + open_folder(music_dir));
        }
        ImGui::SameLine(0, 6);
        if (skin::button("Themes folder", ImVec2(0, 0), colors)) {
          say("Themes folder: " + open_folder(theme_dir));
        }
        skin::text(at(10, 70), colors.dim, "xfade", 0);
        ImGui::SetCursorScreenPos(at(64, 68));
        ImGui::SetNextItemWidth(opt_w - 64 - 100);
        auto cf = static_cast<float>(kept.crossfade);
        if (ImGui::SliderFloat("##xfade", &cf, 0, 12, "")) {
          kept.crossfade = std::round(cf * 2) / 2.0;
          player.set_crossfade(kept.crossfade);
        }
        if (ImGui::IsItemHovered()) {
          ImGui::SetTooltip(
              "Blend the end of a song into the start of the next. Off: the next one just "
              "follows.");
        }
        skin::text(at(opt_w - 90, 70), colors.fg,
                   kept.crossfade < 0.25
                       ? "off"
                       : (std::to_string(kept.crossfade).substr(0, 4) + " s").c_str(),
                   18);
        skin::text(at(10, 100), colors.dim, "theme", 0);
        ImGui::SetCursorScreenPos(at(64, 98));
        ImGui::SetNextItemWidth(220);
        if (ImGui::BeginCombo("##theme", kept.theme.c_str())) {
          for (const dva::Theme& t : themes) {
            if (ImGui::Selectable(t.name.c_str(), t.name == kept.theme)) {
              kept.theme = t.name;
              colors = skin::make_colors(t);
              skin::apply_style(colors);
            }
          }
          ImGui::EndCombo();
        }
      }
      skin::panel_end();

      ImGui::SetCursorPos(list_at);
      skin::panel_begin("songs", ImVec2(list_w, list_h), "Playlist", colors);
      {
        const ImVec2 base = ImGui::GetCursorScreenPos();
        const std::string count =
            std::to_string(list.all.size()) + (list.all.size() == 1 ? " song" : " songs");
        skin::text(ImVec2(base.x + list_w - 16 - ImGui::CalcTextSize(count.c_str()).x, base.y + 6),
                   colors.on_accent, count.c_str(), 0);
        ImGui::SetCursorScreenPos(ImVec2(base.x + 10, base.y + 34));
        ImGui::BeginChild("##songs", ImVec2(list_w - 20, list_h - 44), ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoBackground);
        // the songs as a tree of folders (music/Artist/Album/...): a folder row opens and closes on
        // a click; the one holding the playing song opens by itself
        if (current != revealed) {
          revealed = current;
          if (current >= 0) {
            for (const auto& d : list.dirs[static_cast<std::size_t>(current)]) {
              open_dirs.insert(d.key);
            }
          }
          scroll_to_current = true;
        }
        std::vector<std::string>
            shown;  // the folder keys of the previous row, to see which folder rows come next
        std::map<std::string, int> numbered;
        for (std::size_t i = 0; i < list.all.size(); i++) {
          const auto& dirs = list.dirs[i];
          std::size_t closed_at =
              dirs.size();  // the first folder above this song that is shut (== size: none)
          for (std::size_t d = 0; d < dirs.size(); d++) {
            if (!open_dirs.contains(dirs[d].key)) {
              closed_at = d;
              break;
            }
          }
          std::size_t common = 0;
          while (common < shown.size() && common < dirs.size() &&
                 shown[common] == dirs[common].key) {
            common++;
          }
          shown.clear();
          for (const auto& d : dirs) {
            shown.push_back(d.key);
          }
          for (std::size_t d = common; d < dirs.size() && d <= closed_at; d++) {
            const bool open = open_dirs.contains(dirs[d].key);
            const std::string label =
                std::string(d * 2, ' ') + (open ? "v " : "> ") + dirs[d].name + "/  (" +
                std::to_string(list.songs_in[dirs[d].key]) + ")##d" + dirs[d].key;
            ImGui::PushStyleColor(ImGuiCol_Text, colors.v_cyan);
            if (ImGui::Selectable(label.c_str(), false)) {
              if (open) {
                open_dirs.erase(dirs[d].key);
              } else {
                open_dirs.insert(dirs[d].key);
              }
            }
            ImGui::PopStyleColor();
          }
          const std::string folder_key = dirs.empty() ? std::string() : dirs.back().key;
          const int n = ++numbered[folder_key];
          if (closed_at < dirs.size()) {
            continue;
          }
          char num[16];
          std::snprintf(num, sizeof num, "%02d  ", n);
          const bool is_current = std::cmp_equal(i, current);
          ImGui::PushStyleColor(ImGuiCol_Text, is_current ? colors.v_pink : colors.v_fg);
          const std::string label = std::string(dirs.size() * 2, ' ') + (is_current ? "> " : "  ") +
                                    num + display_name(list.all[i]) + "##" + std::to_string(i);
          if (ImGui::Selectable(label.c_str(), is_current, ImGuiSelectableFlags_AllowDoubleClick) &&
              ImGui::IsMouseDoubleClicked(0)) {
            play(static_cast<int>(i));
          }
          if (is_current && scroll_to_current) {
            ImGui::SetScrollHereY(0.5F);
            scroll_to_current = false;
          }
          ImGui::PopStyleColor();
        }
        if (list.all.empty()) {
          ImGui::TextDisabled("Put songs in the music folder, or start the program with a song.");
        }
        ImGui::EndChild();
      }
      skin::panel_end();
      if (now < notice_until) {
        skin::text(ImVec2(margin, display.y - 22), colors.warn, notice.c_str(), 18);
      }
    }
    ImGui::End();
    skin::scanlines(colors, display);
    if (touched) {
      change(s);
    }

    ImGui::Render();
    int dw = 0;
    int dh = 0;
    SDL_GL_GetDrawableSize(window, &dw, &dh);
    glViewport(0, 0, dw, dh);
    glClearColor(colors.v_deep.x, colors.v_deep.y, colors.v_deep.z, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    frames++;

    if (headless && frames >= shot_frames) {
      std::vector<std::uint8_t> px(static_cast<std::size_t>(dw) * static_cast<std::size_t>(dh) * 4);
      glReadPixels(0, 0, dw, dh, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
      write_ppm(shot_file, px, dw, dh);
      std::printf("saved %s (%dx%d) after %ld frames; %s, position %.2f s\n", shot_file.c_str(), dw,
                  dh, frames, st.path.empty() ? "nothing loaded" : st.path.c_str(), st.position);
      break;
    }
    SDL_GL_SwapWindow(window);
    if (headless) {
      std::vector<float> sink(2 * 800);
      player.render(sink.data(), 800);
      SDL_Delay(8);
    }  // no device in a screenshot run: play it silently ourselves

    if (!same_kept(kept, saved_copy)) {
      if (changed_at < 0) {
        changed_at = now;
      } else if (now - changed_at > 1500 && keep_settings) {
        std::string err;
        if (!save_kept(settings_file, kept, err)) {
          say("settings could not be saved: " + err);
        }
        saved_copy = kept;
        changed_at = -1;
      }
    }
    std::string title = "DejaVuAmp";
    if (!st.tags.title.empty() || !st.path.empty()) {
      title +=
          " - " + dva::ticker_text(st.tags.title.empty() ? display_name(st.path) : st.tags.title,
                                   st.tags.artist);
    }
    if (frames % 30 == 0) {
      SDL_SetWindowTitle(window, title.c_str());
    }
  }
  if (keep_settings && !same_kept(kept, saved_copy)) {
    std::string err;
    save_kept(settings_file, kept, err);
  }
  output.stop();
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplSDL2_Shutdown();
  ImGui::DestroyContext();
  SDL_GL_DeleteContext(gl);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}

// SDL renames main to SDL_main on Windows and needs exactly this signature to link.
int main(int argc, char* argv[]) {
  try {
    return run(argc, argv);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "fatal: %s\n", e.what());
  } catch (...) {
    std::fprintf(stderr, "fatal: unknown error\n");
  }
  return 1;
}
