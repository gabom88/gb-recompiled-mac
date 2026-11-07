// GB Recompiled Launcher
//
// A small graphical front end for gbrecomp: pick (or drop) a Game Boy ROM,
// press "Recompilar", and the launcher runs the whole pipeline for you
//
//     gbrecomp <rom> -o <project>   ->   cmake -G Ninja   ->   ninja
//
// then lists the game in a library with a "Jugar" button. Every game keeps its
// battery saves (.sav/.rtc) and savestates (F5/F8) in its own folder OUTSIDE
// the build tree (passed with --save-dir), so recompiling never erases them.
//
// SDL2 + Dear ImGui (SDL_Renderer backend), the same stack the runtime vendors.
// POSIX only (macOS, Linux).

#include <SDL.h>
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

// ============================================================================
// Small utilities
// ============================================================================

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static bool is_executable_file(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec) && access(p.c_str(), X_OK) == 0;
}

static std::string which(const std::string& tool) {
    const char* path = getenv("PATH");
    if (!path) return "";
    std::stringstream ss(path);
    std::string dir;
    while (std::getline(ss, dir, ':')) {
        if (dir.empty()) continue;
        fs::path cand = fs::path(dir) / tool;
        if (is_executable_file(cand)) return cand.string();
    }
    return "";
}

static std::string run_capture(const std::string& cmd) {
    std::string out;
    FILE* f = popen(cmd.c_str(), "r");
    if (!f) return out;
    char buf[512];
    while (fgets(buf, sizeof buf, f)) out += buf;
    pclose(f);
    return out;
}

static std::string shell_quote(const std::string& s) {
    std::string r = "'";
    for (char c : s) {
        if (c == '\'') r += "'\\''";
        else r += c;
    }
    return r + "'";
}

static std::string human_time(time_t t) {
    if (t <= 0) return "nunca";
    char buf[64];
    strftime(buf, sizeof buf, "%d/%m/%Y %H:%M", localtime(&t));
    return buf;
}

// ---- SHA-1 (ROM identity) --------------------------------------------------
static std::string sha1_hex(const std::vector<uint8_t>& data) {
    uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    std::vector<uint8_t> m(data);
    const uint64_t bit_len = (uint64_t)data.size() * 8;
    m.push_back(0x80);
    while (m.size() % 64 != 56) m.push_back(0);
    for (int i = 7; i >= 0; --i) m.push_back((uint8_t)(bit_len >> (i * 8)));
    auto rol = [](uint32_t v, int n) { return (v << n) | (v >> (32 - n)); };
    for (size_t off = 0; off < m.size(); off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t)m[off + 4 * i] << 24 | (uint32_t)m[off + 4 * i + 1] << 16 |
                   (uint32_t)m[off + 4 * i + 2] << 8 | (uint32_t)m[off + 4 * i + 3];
        for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20)      { f = (b & c) | (~b & d);           k = 0x5A827999u; }
            else if (i < 40) { f = b ^ c ^ d;                    k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d);  k = 0x8F1BBCDCu; }
            else             { f = b ^ c ^ d;                    k = 0xCA62C1D6u; }
            uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    char out[41];
    for (int i = 0; i < 5; ++i) snprintf(out + i * 8, 9, "%08x", h[i]);
    return out;
}

// ============================================================================
// Locations
// ============================================================================

struct Paths {
    fs::path data;       // library root
    fs::path games;      // data/games/<id>/{rom.gb, game.ini, project/}
    fs::path saves;      // data/saves/<id>/   (.sav, .rtc, .stateN)
    fs::path gbrecomp;   // recompiler binary
    fs::path runtime;    // runtime snapshot source
};

static Paths g_paths;

static fs::path find_first(const std::vector<fs::path>& cands, bool want_dir) {
    std::error_code ec;
    for (const auto& c : cands) {
        if (c.empty()) continue;
        if (want_dir ? fs::is_regular_file(c / "include" / "gbrt.h", ec) : is_executable_file(c))
            return fs::weakly_canonical(c, ec);
    }
    return {};
}

static void init_paths() {
    // Library location (GBRL_DATA_DIR overrides, e.g. for tests).
    if (const char* d = getenv("GBRL_DATA_DIR"); d && *d) {
        g_paths.data = d;
    } else {
        const char* home = getenv("HOME");
        fs::path h = home ? home : ".";
#if defined(__APPLE__)
        g_paths.data = h / "Library" / "Application Support" / "GB Recompiled";
#else
        const char* xdg = getenv("XDG_DATA_HOME");
        g_paths.data = (xdg && *xdg) ? fs::path(xdg) / "gb-recompiled"
                                     : h / ".local" / "share" / "gb-recompiled";
#endif
    }
    g_paths.games = g_paths.data / "games";
    g_paths.saves = g_paths.data / "saves";
    std::error_code ec;
    fs::create_directories(g_paths.games, ec);
    fs::create_directories(g_paths.saves, ec);

    // Tools: inside the .app bundle (Contents/Resources, which is what
    // SDL_GetBasePath returns for a bundle) or next to the launcher in a
    // repository build (build/bin/ -> repo/runtime).
    fs::path base;
    if (char* b = SDL_GetBasePath()) { base = b; SDL_free(b); }
    const char* env_gb = getenv("GBRL_GBRECOMP");
    const char* env_rt = getenv("GBRL_RUNTIME");
    g_paths.gbrecomp = find_first({env_gb ? fs::path(env_gb) : fs::path(),
                                   base / "gbrecomp", base / ".." / "Resources" / "gbrecomp"},
                                  false);
    g_paths.runtime = find_first({env_rt ? fs::path(env_rt) : fs::path(),
                                  base / "runtime", base / ".." / "Resources" / "runtime",
                                  base / ".." / ".." / "runtime"},
                                 true);
}

// A Finder-launched app gets a minimal PATH; make Homebrew's tools visible.
static void extend_path() {
    std::string p = getenv("PATH") ? getenv("PATH") : "/usr/bin:/bin";
    const char* extra[] = {"/opt/homebrew/bin", "/usr/local/bin"};
    for (const char* e : extra) {
        if (("" + p + ":").find(std::string(e) + ":") == std::string::npos) p = std::string(e) + ":" + p;
    }
    setenv("PATH", p.c_str(), 1);
}

// ============================================================================
// Known ROMs: verified extra entry points (see the TetrisGBRecomp ports)
// ============================================================================

struct KnownRom {
    const char* sha1;
    const char* name;
    std::vector<std::string> entry_points;   // "bank:addr"
    const char* note;
};

static const std::vector<KnownRom>& known_roms() {
    static const std::vector<KnownRom> k = {
        {"74591cc9501af93873f9a5d3eb12da12c0723bbc", "Tetris (V1.1)",
         {"0:006B", "0:01D8", "0:02A6", "0:02F8", "0:0825", "0:08E4", "0:148C", "0:1A07",
          "1:7FF0", "1:7FF3"},
         "Música A-TYPE: Korobeiniki. Puntos de entrada verificados incluidos."},
        {"3f2a6407c9900ad5817ee1cfb3609c5ee17400fc", "Tetris (V1.0)",
         {"0:018E", "0:0257", "0:0325", "0:0377", "0:0892", "0:0953", "0:14F0", "0:1A6B",
          "1:6641", "1:7FF0", "1:7FF3"},
         "Versión original de 1989 (otra música en A-TYPE). Puntos de entrada verificados incluidos."},
    };
    return k;
}

static const KnownRom* find_known(const std::string& sha1) {
    for (const auto& k : known_roms())
        if (sha1 == k.sha1) return &k;
    return nullptr;
}

// ============================================================================
// Game library
// ============================================================================

struct Game {
    std::string id;           // folder name
    std::string title;        // display name
    std::string header_title; // ROM header title
    std::string sha1;
    std::string source_name;  // original file name
    std::string rom_file;     // ROM copy inside the game folder (names the executable)
    bool cgb = false;
    bool battery = false;
    std::string exe;          // absolute path of the built executable ("" if none)
    time_t built_at = 0;
    std::string status;       // "ok", "failed", "never"
};

static std::vector<Game> g_games;
static int g_selected = -1;

static fs::path game_dir(const Game& g) { return g_paths.games / g.id; }
static fs::path save_dir(const Game& g) { return g_paths.saves / g.id; }

static void save_game_ini(const Game& g) {
    std::ofstream o(game_dir(g) / "game.ini");
    o << "title=" << g.title << "\n"
      << "header_title=" << g.header_title << "\n"
      << "sha1=" << g.sha1 << "\n"
      << "source=" << g.source_name << "\n"
      << "rom_file=" << g.rom_file << "\n"
      << "cgb=" << (g.cgb ? 1 : 0) << "\n"
      << "battery=" << (g.battery ? 1 : 0) << "\n"
      << "exe=" << g.exe << "\n"
      << "built_at=" << (long long)g.built_at << "\n"
      << "status=" << g.status << "\n";
}

static bool load_game_ini(const fs::path& dir, Game& g) {
    std::ifstream in(dir / "game.ini");
    if (!in) return false;
    std::map<std::string, std::string> kv;
    std::string line;
    while (std::getline(in, line)) {
        size_t eq = line.find('=');
        if (eq != std::string::npos) kv[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
    g.id = dir.filename().string();
    g.title = kv["title"];
    g.header_title = kv["header_title"];
    g.sha1 = kv["sha1"];
    g.source_name = kv["source"];
    g.rom_file = kv["rom_file"].empty() ? "rom.gb" : kv["rom_file"];
    g.cgb = kv["cgb"] == "1";
    g.battery = kv["battery"] == "1";
    g.exe = kv["exe"];
    g.built_at = (time_t)atoll(kv["built_at"].c_str());
    g.status = kv["status"].empty() ? "never" : kv["status"];
    if (!g.exe.empty() && !is_executable_file(g.exe)) { g.exe.clear(); if (g.status == "ok") g.status = "never"; }
    return !g.title.empty();
}

static void reload_library(const std::string& select_id = "") {
    std::string keep = select_id;
    if (keep.empty() && g_selected >= 0 && g_selected < (int)g_games.size()) keep = g_games[g_selected].id;
    g_games.clear();
    std::error_code ec;
    for (auto& e : fs::directory_iterator(g_paths.games, ec)) {
        if (!e.is_directory()) continue;
        Game g;
        if (load_game_ini(e.path(), g)) g_games.push_back(g);
    }
    std::sort(g_games.begin(), g_games.end(),
              [](const Game& a, const Game& b) { return a.title < b.title; });
    g_selected = g_games.empty() ? -1 : 0;
    for (size_t i = 0; i < g_games.size(); ++i)
        if (g_games[i].id == keep) g_selected = (int)i;
}

static std::string slug(const std::string& s) {
    std::string r;
    for (char c : s) {
        if (isalnum((unsigned char)c)) r += (char)tolower((unsigned char)c);
        else if (!r.empty() && r.back() != '-') r += '-';
    }
    while (!r.empty() && r.back() == '-') r.pop_back();
    return r.empty() ? "game" : r;
}

// Imports a ROM into the library. Returns the game id or "" (error in `err`).
static std::string import_rom(const std::string& path, std::string& err) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { err = "No se pudo abrir el archivo."; return ""; }
    std::vector<uint8_t> rom((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (rom.size() < 0x150 || rom.size() > 8 * 1024 * 1024) {
        err = "No parece una ROM de Game Boy (tamaño " + std::to_string(rom.size()) + " bytes).";
        return "";
    }
    // Header title: 0x134..0x143 (15 chars when 0x143 is a CGB flag).
    const bool cgb = (rom[0x143] & 0x80) != 0;
    std::string title;
    for (int i = 0x134; i < (cgb ? 0x143 : 0x144); ++i) {
        if (rom[i] == 0) break;
        if (rom[i] >= 32 && rom[i] < 127) title += (char)rom[i];
    }
    title = trim(title);
    static const uint8_t kBattery[] = {0x03, 0x06, 0x09, 0x0D, 0x0F, 0x10, 0x13, 0x1B, 0x1E, 0x22, 0xFF};
    bool battery = std::find(std::begin(kBattery), std::end(kBattery), rom[0x147]) != std::end(kBattery);

    const std::string sha = sha1_hex(rom);
    for (const auto& g : g_games)
        if (g.sha1 == sha) { err = "Esta ROM ya está en tu biblioteca (" + g.title + ")."; return g.id; }

    Game g;
    g.header_title = title.empty() ? "SIN TITULO" : title;
    const KnownRom* k = find_known(sha);
    g.title = k ? k->name : (title.empty() ? fs::path(path).stem().string() : title);
    g.id = slug(g.header_title) + "-" + sha.substr(0, 8);
    g.sha1 = sha;
    g.source_name = fs::path(path).filename().string();
    {   // gbrecomp names the executable (and savestates) after the ROM file
        std::string base = slug(g.header_title);
        std::replace(base.begin(), base.end(), '-', '_');
        g.rom_file = base + (cgb ? ".gbc" : ".gb");
    }
    g.cgb = cgb;
    g.battery = battery;
    g.status = "never";
    std::error_code ec;
    fs::create_directories(game_dir(g), ec);
    fs::create_directories(save_dir(g), ec);
    std::ofstream out(game_dir(g) / g.rom_file, std::ios::binary);
    out.write((const char*)rom.data(), (std::streamsize)rom.size());
    if (!out) { err = "No se pudo copiar la ROM a la biblioteca."; return ""; }
    out.close();
    save_game_ini(g);
    return g.id;
}

// ============================================================================
// Child processes
// ============================================================================

struct Job {
    std::mutex mu;
    std::deque<std::string> log;
    std::string step = "";
    float progress = 0.0f;
    std::atomic<bool> running{false};
    std::atomic<bool> cancel{false};
    std::atomic<pid_t> child{-1};
    bool finished = false;
    bool success = false;
    std::string result_msg;
    std::string game_id;
    std::thread th;

    void add(const std::string& line) {
        std::lock_guard<std::mutex> lk(mu);
        log.push_back(line);
        while (log.size() > 4000) log.pop_front();
    }
    void set_step(const std::string& s, float p) {
        std::lock_guard<std::mutex> lk(mu);
        step = s; progress = p;
    }
};

static Job g_job;

// Runs argv in cwd, streaming stdout+stderr lines to `on_line`. Returns the
// exit status (or -1 if it could not start / was cancelled).
template <class F>
static int run_streaming(const std::vector<std::string>& argv, const fs::path& cwd, F on_line) {
    int fds[2];
    if (pipe(fds) != 0) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return -1; }
    if (pid == 0) {
        setpgid(0, 0);
        dup2(fds[1], 1); dup2(fds[1], 2);
        close(fds[0]); close(fds[1]);
        if (!cwd.empty() && chdir(cwd.c_str()) != 0) _exit(126);
        std::vector<char*> a;
        for (auto& s : argv) a.push_back(const_cast<char*>(s.c_str()));
        a.push_back(nullptr);
        execvp(a[0], a.data());
        fprintf(stderr, "No se pudo ejecutar %s: %s\n", a[0], strerror(errno));
        _exit(127);
    }
    close(fds[1]);
    g_job.child = pid;
    std::string buf;
    char chunk[4096];
    for (;;) {
        ssize_t n = read(fds[0], chunk, sizeof chunk);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        buf.append(chunk, (size_t)n);
        size_t pos;
        while ((pos = buf.find_first_of("\r\n")) != std::string::npos) {
            std::string line = buf.substr(0, pos);
            buf.erase(0, pos + 1);
            if (!line.empty()) on_line(line);
        }
    }
    if (!buf.empty()) on_line(buf);
    close(fds[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    g_job.child = -1;
    if (g_job.cancel) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static void cancel_job() {
    g_job.cancel = true;
    pid_t p = g_job.child;
    if (p > 0) kill(-p, SIGTERM);
}

static void finish_job(bool ok, const std::string& msg) {
    std::lock_guard<std::mutex> lk(g_job.mu);
    g_job.finished = true;
    g_job.success = ok;
    g_job.result_msg = msg;
}

static void start_job(const std::string& game_id, std::function<void()> body) {
    if (g_job.running) return;
    if (g_job.th.joinable()) g_job.th.join();
    {
        std::lock_guard<std::mutex> lk(g_job.mu);
        g_job.log.clear();
        g_job.step.clear();
        g_job.progress = 0.0f;
        g_job.finished = false;
        g_job.success = false;
        g_job.result_msg.clear();
        g_job.game_id = game_id;
    }
    g_job.cancel = false;
    g_job.running = true;
    g_job.th = std::thread([body]() {
        body();
        g_job.running = false;
    });
}

// ---- The recompilation pipeline --------------------------------------------
static std::string executable_name_from_cmake(const fs::path& cmake_lists) {
    std::ifstream in(cmake_lists);
    std::string line;
    while (std::getline(in, line)) {
        size_t p = line.find("add_executable(");
        if (p == std::string::npos) continue;
        std::string rest = line.substr(p + 15);
        size_t end = rest.find_first_of(" \t)");
        std::string name = trim(rest.substr(0, end));
        if (!name.empty() && name[0] != '$') return name;
    }
    return "";
}

static void recompile_game(Game g) {
    const fs::path dir = game_dir(g);
    const fs::path project = dir / "project";
    const fs::path build = project / "build";
    std::ofstream logf(dir / "build.log");
    auto line_cb = [&](const std::string& l) { g_job.add(l); logf << l << "\n"; logf.flush(); };

    g.status = "failed";
    save_game_ini(g);

    // 1. ROM -> C
    g_job.set_step("1/3  Traduciendo la ROM a código C (gbrecomp)", 0.03f);
    std::error_code ec;
    fs::remove_all(project, ec);   // always regenerate cleanly (saves live elsewhere)
    std::vector<std::string> args = {g_paths.gbrecomp.string(), (dir / g.rom_file).string(),
                                     "-o", project.string()};
    if (!g_paths.runtime.empty()) { args.push_back("--runtime-dir"); args.push_back(g_paths.runtime.string()); }
    if (const KnownRom* k = find_known(g.sha1)) {
        for (const auto& ep : k->entry_points) { args.push_back("--add-entry-point"); args.push_back(ep); }
        line_cb(std::string("[launcher] ROM reconocida: ") + k->name + " (puntos de entrada verificados)");
    }
    int rc = run_streaming(args, dir, [&](const std::string& l) {
        line_cb(l);
        if (l.find("Analyzing") != std::string::npos) g_job.set_step("1/3  Analizando el código del juego", 0.08f);
        else if (l.find("Building IR") != std::string::npos) g_job.set_step("1/3  Construyendo la representación intermedia", 0.14f);
        else if (l.find("Generating C") != std::string::npos) g_job.set_step("1/3  Generando código C", 0.18f);
    });
    if (g_job.cancel) { finish_job(false, "Cancelado."); save_game_ini(g); return; }
    if (rc != 0 || !fs::exists(project / "CMakeLists.txt")) {
        finish_job(false, "gbrecomp no pudo traducir la ROM (código " + std::to_string(rc) + "). Mira los detalles.");
        save_game_ini(g);
        return;
    }

    // 2. Configure
    g_job.set_step("2/3  Preparando la compilación (CMake)", 0.22f);
    rc = run_streaming({"cmake", "-G", "Ninja", "-S", project.string(), "-B", build.string(),
                        "-DCMAKE_BUILD_TYPE=Release"},
                       dir, line_cb);
    if (g_job.cancel) { finish_job(false, "Cancelado."); save_game_ini(g); return; }
    if (rc != 0) {
        finish_job(false, "CMake falló (código " + std::to_string(rc) + "). ¿Están instalados SDL2 y Ninja?");
        save_game_ini(g);
        return;
    }

    // 3. Build
    g_job.set_step("3/3  Compilando el juego nativo (Ninja)", 0.26f);
    rc = run_streaming({"ninja", "-C", build.string()}, dir, [&](const std::string& l) {
        line_cb(l);
        int a = 0, b = 0;
        if (sscanf(l.c_str(), "[%d/%d]", &a, &b) == 2 && b > 0) {
            char s[96];
            snprintf(s, sizeof s, "3/3  Compilando el juego nativo (%d de %d)", a, b);
            g_job.set_step(s, 0.26f + 0.74f * (float)a / (float)b);
        }
    });
    if (g_job.cancel) { finish_job(false, "Cancelado."); save_game_ini(g); return; }
    if (rc != 0) {
        finish_job(false, "La compilación falló (código " + std::to_string(rc) + "). Mira los detalles.");
        save_game_ini(g);
        return;
    }

    const std::string exe_name = executable_name_from_cmake(project / "CMakeLists.txt");
    fs::path exe = build / exe_name;
    if (exe_name.empty() || !is_executable_file(exe)) {
        finish_job(false, "Compiló, pero no encontré el ejecutable del juego.");
        save_game_ini(g);
        return;
    }
    g.exe = exe.string();
    g.built_at = time(nullptr);
    g.status = "ok";
    save_game_ini(g);
    g_job.set_step("Listo", 1.0f);
    finish_job(true, "¡" + g.title + " está listo para jugar!");
}

static void install_dependencies(std::vector<std::string> pkgs) {
    g_job.set_step("Instalando con Homebrew: puede tardar varios minutos", 0.05f);
    std::vector<std::string> args = {"brew", "install"};
    args.insert(args.end(), pkgs.begin(), pkgs.end());
    int rc = run_streaming(args, "", [](const std::string& l) { g_job.add(l); });
    if (g_job.cancel) { finish_job(false, "Cancelado."); return; }
    if (rc == 0) { g_job.set_step("Listo", 1.0f); finish_job(true, "Dependencias instaladas."); }
    else finish_job(false, "Homebrew no pudo instalar las dependencias (código " + std::to_string(rc) + ").");
}

// ---- Playing ------------------------------------------------------------------
struct Running { pid_t pid; std::string id; std::string title; };
static std::vector<Running> g_running;
static std::string g_toast;          // short status message under the header
static Uint32 g_toast_until = 0;

static void toast(const std::string& s, Uint32 ms = 6000) { g_toast = s; g_toast_until = SDL_GetTicks() + ms; }

static void play_game(const Game& g) {
    if (g.exe.empty()) return;
    std::error_code ec;
    fs::create_directories(save_dir(g), ec);
    const fs::path cwd = fs::path(g.exe).parent_path();
    const fs::path runlog = game_dir(g) / "last_run.log";
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        int fd = open(runlog.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); close(fd); }
        if (chdir(cwd.c_str()) != 0) _exit(126);
        const std::string sd = save_dir(g).string();
        execl(g.exe.c_str(), g.exe.c_str(), "--save-dir", sd.c_str(), (char*)nullptr);
        _exit(127);
    }
    if (pid > 0) {
        g_running.push_back({pid, g.id, g.title});
        toast("Abriendo " + g.title + "...");
    }
}

static void reap_games() {
    for (size_t i = 0; i < g_running.size();) {
        int st = 0;
        pid_t r = waitpid(g_running[i].pid, &st, WNOHANG);
        if (r == g_running[i].pid) {
            const bool bad = !WIFEXITED(st) || WEXITSTATUS(st) != 0;
            if (bad) toast(g_running[i].title + " se cerró con un error. Mira last_run.log en la carpeta del juego.", 10000);
            g_running.erase(g_running.begin() + (long)i);
        } else {
            ++i;
        }
    }
}

static bool is_running(const std::string& id) {
    for (auto& r : g_running) if (r.id == id) return true;
    return false;
}

static void open_in_finder(const fs::path& p) {
#if defined(__APPLE__)
    std::string cmd = "open " + shell_quote(p.string()) + " &";
#else
    std::string cmd = "xdg-open " + shell_quote(p.string()) + " >/dev/null 2>&1 &";
#endif
    if (system(cmd.c_str()) != 0) toast("No se pudo abrir " + p.string());
}

// ---- File picker (runs on a thread so the window keeps repainting) ----------
static std::atomic<bool> g_picking{false};
static std::mutex g_pick_mu;
static std::vector<std::string> g_pending_imports;

static void pick_rom_async() {
    if (g_picking) return;
    g_picking = true;
    std::thread([]() {
        std::string out;
#if defined(__APPLE__)
        out = run_capture("osascript -e 'POSIX path of (choose file with prompt "
                          "\"Elige una ROM de Game Boy (.gb o .gbc)\")' 2>/dev/null");
#else
        if (!which("zenity").empty())
            out = run_capture("zenity --file-selection --title='Elige una ROM de Game Boy' "
                              "--file-filter='ROM de Game Boy | *.gb *.gbc *.GB *.GBC' 2>/dev/null");
#endif
        out = trim(out);
        if (!out.empty()) {
            std::lock_guard<std::mutex> lk(g_pick_mu);
            g_pending_imports.push_back(out);
        }
        g_picking = false;
    }).detach();
}

// ============================================================================
// Dependency check
// ============================================================================

struct Deps {
    bool gbrecomp = false, runtime = false, cmake = false, ninja = false, sdl2 = false, brew = false;
    std::vector<std::string> missing_pkgs;
    bool ok() const { return gbrecomp && runtime && cmake && ninja && sdl2; }
};
static Deps g_deps;

static void check_deps() {
    Deps d;
    d.gbrecomp = !g_paths.gbrecomp.empty();
    d.runtime = !g_paths.runtime.empty();
    d.cmake = !which("cmake").empty();
    d.ninja = !which("ninja").empty();
    d.sdl2 = !which("sdl2-config").empty() ||
             system("pkg-config --exists sdl2 >/dev/null 2>&1") == 0;
    d.brew = !which("brew").empty();
    if (!d.cmake) d.missing_pkgs.push_back("cmake");
    if (!d.ninja) d.missing_pkgs.push_back("ninja");
    if (!d.sdl2) d.missing_pkgs.push_back("sdl2");
    g_deps = d;
}

// ============================================================================
// Look: the classic DMG-01 palette on a light-grey case
// ============================================================================

static ImVec4 hex(uint32_t rgb, float a = 1.0f) {
    return ImVec4(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, a);
}
namespace pal {
static const ImVec4 bg        = hex(0xB9BAB3);   // case (shadow)
static const ImVec4 card      = hex(0xD9DAD3);   // panels
static const ImVec4 card_hi   = hex(0xE4E5DE);
static const ImVec4 control   = hex(0xC3C4BD);
static const ImVec4 control_h = hex(0xB0B1AA);
static const ImVec4 border    = hex(0x8F908A);
static const ImVec4 ink       = hex(0x22261F);
static const ImVec4 muted     = hex(0x5A5E57);
static const ImVec4 lcd0      = hex(0x0F380F);   // LCD darkest
static const ImVec4 lcd1      = hex(0x306230);
static const ImVec4 lcd2      = hex(0x8BAC0F);
static const ImVec4 lcd3      = hex(0x9BBC0F);   // LCD lightest
static const ImVec4 navy      = hex(0x2B2F7D);   // wordmark
static const ImVec4 magenta   = hex(0x9B1D4D);   // A/B buttons
static const ImVec4 warn      = hex(0x9B5A00);
}  // namespace pal

static ImFont* g_font_body = nullptr;
static ImFont* g_font_title = nullptr;
static ImFont* g_font_small = nullptr;

static void apply_style(float scale) {
    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    s.WindowRounding = 0; s.ChildRounding = 10; s.FrameRounding = 7; s.PopupRounding = 8;
    s.GrabRounding = 7; s.ScrollbarRounding = 7;
    s.WindowPadding = ImVec2(20, 18); s.FramePadding = ImVec2(14, 8); s.ItemSpacing = ImVec2(10, 9);
    s.ChildBorderSize = 1; s.FrameBorderSize = 1; s.WindowBorderSize = 0;
    s.ScrollbarSize = 14;
    s.ScaleAllSizes(scale);
    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg] = pal::bg;
    c[ImGuiCol_ChildBg] = pal::card;
    c[ImGuiCol_PopupBg] = pal::card_hi;
    c[ImGuiCol_Border] = pal::border;
    c[ImGuiCol_Text] = pal::ink;
    c[ImGuiCol_TextDisabled] = pal::muted;
    c[ImGuiCol_FrameBg] = pal::control;
    c[ImGuiCol_FrameBgHovered] = pal::control_h;
    c[ImGuiCol_FrameBgActive] = pal::control_h;
    c[ImGuiCol_Button] = pal::control;
    c[ImGuiCol_ButtonHovered] = pal::control_h;
    c[ImGuiCol_ButtonActive] = hex(0x9FA099);
    c[ImGuiCol_Header] = hex(0xC8D0A8);
    c[ImGuiCol_HeaderHovered] = hex(0xCBD3AC);
    c[ImGuiCol_HeaderActive] = hex(0xB9C792);
    c[ImGuiCol_PlotHistogram] = pal::lcd1;
    c[ImGuiCol_ScrollbarBg] = hex(0xCDCEC7);
    c[ImGuiCol_ScrollbarGrab] = pal::border;
    c[ImGuiCol_ScrollbarGrabHovered] = pal::muted;
    c[ImGuiCol_CheckMark] = pal::lcd1;
    c[ImGuiCol_NavHighlight] = pal::magenta;
    c[ImGuiCol_Separator] = pal::border;
    c[ImGuiCol_ModalWindowDimBg] = hex(0x22261F, 0.35f);
    c[ImGuiCol_TitleBg] = pal::navy;
    c[ImGuiCol_TitleBgActive] = pal::navy;
    c[ImGuiCol_TitleBgCollapsed] = pal::navy;
}

static ImFont* load_font(const std::vector<const char*>& files, float px) {
    ImGuiIO& io = ImGui::GetIO();
    for (const char* f : files) {
        std::error_code ec;
        if (fs::exists(f, ec)) {
            ImFontConfig cfg;
            cfg.OversampleH = 2;
            if (ImFont* font = io.Fonts->AddFontFromFileTTF(f, px, &cfg, io.Fonts->GetGlyphRangesDefault()))
                return font;
        }
    }
    ImFontConfig cfg;
    cfg.SizePixels = px;
    return io.Fonts->AddFontDefault(&cfg);
}

static void load_fonts(float scale) {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    const std::vector<const char*> regular = {
        "/System/Library/Fonts/Supplemental/Arial.ttf", "/Library/Fonts/Arial.ttf",
        "/System/Library/Fonts/Helvetica.ttc",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf"};
    const std::vector<const char*> bold = {
        "/System/Library/Fonts/Supplemental/Arial Bold.ttf", "/Library/Fonts/Arial Bold.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf"};
    g_font_body = load_font(regular, 16.0f * scale);
    g_font_small = load_font(regular, 13.5f * scale);
    g_font_title = load_font(bold, 26.0f * scale);
    io.FontGlobalScale = 1.0f / scale;   // fonts rasterised at pixel density
}

// ---- widgets --------------------------------------------------------------------
static bool big_button(const char* label, ImVec2 size, const ImVec4& bg, const ImVec4& fg,
                       bool enabled = true) {
    ImGui::BeginDisabled(!enabled);
    ImGui::PushStyleColor(ImGuiCol_Button, bg);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(bg.x * 1.12f, bg.y * 1.12f, bg.z * 1.12f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(bg.x * 0.85f, bg.y * 0.85f, bg.z * 0.85f, 1));
    ImGui::PushStyleColor(ImGuiCol_Text, fg);
    ImGui::PushStyleColor(ImGuiCol_Border, pal::lcd0);
    bool r = ImGui::Button(label, size);
    ImGui::PopStyleColor(5);
    ImGui::EndDisabled();
    return r;
}

static void heading(const char* s) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetTextLineHeight();
    dl->AddRectFilled(ImVec2(p.x, p.y + 2), ImVec2(p.x + 4, p.y + h - 2), ImGui::GetColorU32(pal::navy), 2);
    ImGui::SetCursorScreenPos(ImVec2(p.x + 12, p.y));
    ImGui::PushStyleColor(ImGuiCol_Text, pal::navy);
    ImGui::TextUnformatted(s);
    ImGui::PopStyleColor();
}

// A tiny DMG "screen" tile with the game's initials.
static void game_tile(const Game& g, float size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    dl->AddRectFilled(p, ImVec2(p.x + size, p.y + size), ImGui::GetColorU32(hex(0x5D5E6B)), 8);
    const float m = size * 0.12f;
    dl->AddRectFilled(ImVec2(p.x + m, p.y + m), ImVec2(p.x + size - m, p.y + size - m),
                      ImGui::GetColorU32(g.cgb ? hex(0xA6C8A0) : pal::lcd3), 3);
    std::string ini;
    for (char c : g.title) {
        if (isalnum((unsigned char)c)) { ini += (char)toupper((unsigned char)c); if (ini.size() == 2) break; }
    }
    ImGui::PushFont(g_font_title);
    ImVec2 ts = ImGui::CalcTextSize(ini.c_str());
    dl->AddText(ImVec2(p.x + (size - ts.x) * 0.5f, p.y + (size - ts.y) * 0.5f),
                ImGui::GetColorU32(pal::lcd0), ini.c_str());
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(size, size));
}

// ============================================================================
// UI
// ============================================================================

static bool g_show_log = false;
static bool g_confirm_delete = false;
static bool g_delete_saves = false;
static std::string g_import_error;

static void begin_recompile(const Game& g) {
    if (!g_deps.ok() || g_job.running) return;
    Game copy = g;
    start_job(g.id, [copy]() { recompile_game(copy); });
}

static void process_imports() {
    std::vector<std::string> todo;
    {
        std::lock_guard<std::mutex> lk(g_pick_mu);
        todo.swap(g_pending_imports);
    }
    for (const auto& path : todo) {
        std::string err;
        std::string id = import_rom(path, err);
        if (!err.empty()) g_import_error = err;
        if (!id.empty()) {
            reload_library(id);
            if (err.empty() && g_selected >= 0) {
                toast("Añadido: " + g_games[g_selected].title);
                begin_recompile(g_games[g_selected]);   // recompile right away
            }
        }
    }
}

static void draw_deps_banner(float w) {
    if (g_deps.ok()) return;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, hex(0xE9DFC6));
    ImGui::BeginChild("deps", ImVec2(w, 0), ImGuiChildFlags_Border | ImGuiChildFlags_AutoResizeY);
    ImGui::PushStyleColor(ImGuiCol_Text, pal::warn);
    ImGui::TextUnformatted("Falta algo para poder recompilar");
    ImGui::PopStyleColor();
    if (!g_deps.gbrecomp || !g_deps.runtime)
        ImGui::TextWrapped("No encuentro el recompilador o su runtime. Vuelve a instalar la app con macos/install.sh.");
    if (!g_deps.missing_pkgs.empty()) {
        std::string list;
        for (auto& p : g_deps.missing_pkgs) list += (list.empty() ? "" : ", ") + p;
        ImGui::TextWrapped("Herramientas que faltan: %s.", list.c_str());
        if (g_deps.brew) {
            if (big_button("Instalar con Homebrew", ImVec2(0, 0), pal::lcd1, pal::lcd3, !g_job.running)) {
                auto pk = g_deps.missing_pkgs;
                start_job("", [pk]() { install_dependencies(pk); });
                g_show_log = true;
            }
            ImGui::SameLine();
            ImGui::TextColored(pal::muted, "En Mac Intel puede tardar 15-40 minutos.");
        } else {
            ImGui::TextWrapped("Instala Homebrew desde https://brew.sh y vuelve a abrir esta app.");
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, 4));
}

static void draw_job_panel(float scale) {
    std::string step, msg;
    float prog;
    bool finished, ok;
    std::vector<std::string> lines;
    {
        std::lock_guard<std::mutex> lk(g_job.mu);
        step = g_job.step; prog = g_job.progress; finished = g_job.finished; ok = g_job.success;
        msg = g_job.result_msg;
        if (g_show_log) lines.assign(g_job.log.begin(), g_job.log.end());
    }
    const bool running = g_job.running;
    if (!running && !finished) return;

    ImGui::Dummy(ImVec2(0, 4 * scale));
    heading(running ? "RECOMPILANDO" : (ok ? "TERMINADO" : "NO SE PUDO COMPLETAR"));
    if (running) {
        ImGui::TextUnformatted(step.c_str());
        ImGui::ProgressBar(prog, ImVec2(-1, 22 * scale), "");
        if (ImGui::Button("Cancelar")) cancel_job();
        ImGui::SameLine();
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, ok ? pal::lcd1 : pal::magenta);
        ImGui::TextWrapped("%s", msg.c_str());
        ImGui::PopStyleColor();
    }
    if (ImGui::Button(g_show_log ? "Ocultar detalles" : "Ver detalles")) g_show_log = !g_show_log;
    if (!running) {
        ImGui::SameLine();
        if (ImGui::Button("Cerrar")) {
            std::lock_guard<std::mutex> lk(g_job.mu);
            g_job.finished = false;
        }
    }
    if (g_show_log) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, hex(0x1E2A1E));
        ImGui::PushStyleColor(ImGuiCol_Text, pal::lcd3);
        ImGui::PushFont(g_font_small);
        ImGui::BeginChild("log", ImVec2(-1, -1), ImGuiChildFlags_Border, ImGuiWindowFlags_HorizontalScrollbar);
        ImGuiListClipper clip;
        clip.Begin((int)lines.size());
        while (clip.Step())
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) ImGui::TextUnformatted(lines[i].c_str());
        if (running && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 40) ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
        ImGui::PopFont();
        ImGui::PopStyleColor(2);
    }
}

static void draw_game_details(Game& g, float scale) {
    const bool building_this = g_job.running && g_job.game_id == g.id;
    const bool playing = is_running(g.id);

    game_tile(g, 92 * scale);
    ImGui::SameLine(0, 16 * scale);
    ImGui::BeginGroup();
    ImGui::PushFont(g_font_title);
    ImGui::TextUnformatted(g.title.c_str());
    ImGui::PopFont();
    ImGui::TextColored(pal::muted, "%s  ·  %s", g.cgb ? "Game Boy Color" : "Game Boy",
                       g.source_name.c_str());
    if (g.status == "ok") ImGui::TextColored(pal::lcd1, "Listo para jugar  ·  compilado el %s", human_time(g.built_at).c_str());
    else if (g.status == "failed") ImGui::TextColored(pal::magenta, "La última recompilación falló");
    else ImGui::TextColored(pal::warn, "Todavía no está compilado");
    ImGui::EndGroup();

    ImGui::Dummy(ImVec2(0, 6 * scale));
    const float bw = 190 * scale, bh = 46 * scale;
    if (big_button(playing ? "Jugando..." : "JUGAR", ImVec2(bw, bh), pal::lcd1, pal::lcd3,
                   g.status == "ok" && !playing && !building_this))
        play_game(g);
    ImGui::SameLine(0, 12 * scale);
    if (big_button(g.status == "ok" ? "Recompilar" : "RECOMPILAR", ImVec2(bw, bh),
                   g.status == "ok" ? pal::control : pal::lcd1, g.status == "ok" ? pal::ink : pal::lcd3,
                   g_deps.ok() && !g_job.running && !playing))
        begin_recompile(g);

    ImGui::Dummy(ImVec2(0, 8 * scale));
    heading("PARTIDAS GUARDADAS");
    ImGui::PushFont(g_font_small);
    ImGui::TextWrapped("Se guardan aparte, en tu carpeta de usuario: recompilar o borrar el juego no las toca.");
    if (g.battery)
        ImGui::TextWrapped("Este cartucho tiene pila: el progreso se guarda como en la consola (al guardar dentro del juego).");
    else
        ImGui::TextWrapped("Este cartucho NO tiene pila: el juego original tampoco guardaba nada al apagarse "
                           "(por ejemplo, los récords de Tetris). Usa los estados: F5 guarda, F8 carga, "
                           "F6/F7 cambian de ranura. En un MacBook con Touch Bar, mantén fn.");
    ImGui::PopFont();
    if (ImGui::Button("Abrir carpeta de partidas")) open_in_finder(save_dir(g));
    ImGui::SameLine();
    if (ImGui::Button("Abrir carpeta del juego")) open_in_finder(game_dir(g));
    ImGui::SameLine();
    ImGui::BeginDisabled(g_job.running || playing);
    if (ImGui::Button("Eliminar de la biblioteca")) { g_confirm_delete = true; g_delete_saves = false; }
    ImGui::EndDisabled();

    if (const KnownRom* k = find_known(g.sha1)) {
        ImGui::Dummy(ImVec2(0, 4 * scale));
        ImGui::PushFont(g_font_small);
        ImGui::TextColored(pal::navy, "%s", k->note);
        ImGui::PopFont();
    }
}

static void draw_delete_modal() {
    if (g_confirm_delete) { ImGui::OpenPopup("Eliminar juego"); g_confirm_delete = false; }
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.96f, 0.96f, 0.93f, 1));   // title bar text
    const bool del_open = ImGui::BeginPopupModal("Eliminar juego", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PopStyleColor();
    if (del_open) {
        if (g_selected >= 0 && g_selected < (int)g_games.size()) {
            Game& g = g_games[g_selected];
            ImGui::Text("¿Eliminar \"%s\" de la biblioteca?", g.title.c_str());
            ImGui::TextColored(pal::muted, "Se borra la copia de la ROM y la compilación.");
            ImGui::Checkbox("Borrar también sus partidas guardadas", &g_delete_saves);
            ImGui::Dummy(ImVec2(0, 6));
            if (big_button("Eliminar", ImVec2(140, 0), pal::magenta, ImVec4(1, 1, 1, 1))) {
                std::error_code ec;
                fs::remove_all(game_dir(g), ec);
                if (g_delete_saves) fs::remove_all(save_dir(g), ec);
                toast("Eliminado: " + g.title);
                reload_library();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancelar", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        } else {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (!g_import_error.empty()) { ImGui::OpenPopup("Aviso"); }
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.96f, 0.96f, 0.93f, 1));
    const bool warn_open = ImGui::BeginPopupModal("Aviso", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PopStyleColor();
    if (warn_open) {
        ImGui::TextUnformatted(g_import_error.c_str());
        if (ImGui::Button("Aceptar", ImVec2(120, 0))) { g_import_error.clear(); ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
}

static void draw_ui(float scale) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::Begin("##main", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings);

    // ---- header ----
    ImGui::PushFont(g_font_title);
    ImGui::PushStyleColor(ImGuiCol_Text, pal::navy);
    ImGui::TextUnformatted("GB Recompiled");
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 9 * scale);
    ImGui::TextColored(pal::muted, "Tus juegos de Game Boy como apps nativas");
    {
        const char* add = g_picking ? "Eligiendo..." : "+  Añadir ROM";
        const float w = 170 * scale;
        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - w);
        ImGui::SetCursorPosY(ImGui::GetStyle().WindowPadding.y);
        if (big_button(add, ImVec2(w, 38 * scale), pal::lcd1, pal::lcd3, !g_picking && !g_job.running))
            pick_rom_async();
    }
    {
        ImVec2 p = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x;
        ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + w, p.y + 2 * scale), ImGui::GetColorU32(ImVec4(pal::navy.x, pal::navy.y, pal::navy.z, 0.7f)));
        ImGui::Dummy(ImVec2(0, 8 * scale));
    }
    if (!g_toast.empty() && SDL_GetTicks() < g_toast_until) {
        ImGui::TextColored(pal::lcd1, "%s", g_toast.c_str());
    }

    draw_deps_banner(ImGui::GetContentRegionAvail().x);

    // ---- body ----
    if (g_games.empty()) {
        ImGui::BeginChild("empty", ImVec2(-1, -1), ImGuiChildFlags_Border);
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        ImGui::Dummy(ImVec2(0, avail.y * 0.30f));
        auto centered = [&](const char* s, ImFont* f, const ImVec4& col) {
            ImGui::PushFont(f);
            float tw = ImGui::CalcTextSize(s).x;
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - tw) * 0.5f);
            ImGui::TextColored(col, "%s", s);
            ImGui::PopFont();
        };
        centered("Tu biblioteca está vacía", g_font_title, pal::navy);
        centered("Arrastra aquí una ROM (.gb o .gbc) o pulsa  \"+ Añadir ROM\".", g_font_body, pal::ink);
        centered("La app la recompila a código nativo y la deja lista para jugar.", g_font_body, pal::muted);
        draw_job_panel(scale);
        ImGui::EndChild();
    } else {
        const float list_w = 300 * scale;
        ImGui::BeginChild("list", ImVec2(list_w, -1), ImGuiChildFlags_Border);
        heading("BIBLIOTECA");
        ImGui::Dummy(ImVec2(0, 2));
        for (int i = 0; i < (int)g_games.size(); ++i) {
            Game& g = g_games[i];
            ImGui::PushID(i);
            const float row_h = 54 * scale;
            ImVec2 p = ImGui::GetCursorScreenPos();
            if (ImGui::Selectable("##row", g_selected == i, 0, ImVec2(0, row_h))) g_selected = i;
            ImGui::SetCursorScreenPos(ImVec2(p.x + 6 * scale, p.y + 6 * scale));
            game_tile(g, row_h - 12 * scale);
            ImGui::SetCursorScreenPos(ImVec2(p.x + row_h + 4 * scale, p.y + 6 * scale));
            ImGui::BeginGroup();
            ImGui::TextUnformatted(g.title.c_str());
            ImGui::PushFont(g_font_small);
            if (g_job.running && g_job.game_id == g.id) ImGui::TextColored(pal::navy, "Recompilando...");
            else if (is_running(g.id)) ImGui::TextColored(pal::lcd1, "Jugando");
            else if (g.status == "ok") ImGui::TextColored(pal::lcd1, "Listo");
            else if (g.status == "failed") ImGui::TextColored(pal::magenta, "Error al compilar");
            else ImGui::TextColored(pal::warn, "Sin compilar");
            ImGui::PopFont();
            ImGui::EndGroup();
            ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + row_h + ImGui::GetStyle().ItemSpacing.y));
            ImGui::PopID();
        }
        ImGui::EndChild();
        ImGui::SameLine(0, 14 * scale);
        ImGui::BeginChild("details", ImVec2(-1, -1), ImGuiChildFlags_Border);
        if (g_selected >= 0 && g_selected < (int)g_games.size()) draw_game_details(g_games[g_selected], scale);
        draw_job_panel(scale);
        ImGui::EndChild();
    }

    draw_delete_modal();
    ImGui::End();
}

// ============================================================================
// main
// ============================================================================

int main(int argc, char** argv) {
    signal(SIGPIPE, SIG_IGN);
    extend_path();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    init_paths();
    check_deps();
    reload_library();

    // Command-line imports (also used by tests): gbrecomp-launcher rom1.gb ...
    for (int i = 1; i < argc; ++i) {
        std::lock_guard<std::mutex> lk(g_pick_mu);
        g_pending_imports.push_back(argv[i]);
    }

    SDL_Window* win = SDL_CreateWindow("GB Recompiled", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       1040, 700,
                                       SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!win) { fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return 1; }
    SDL_SetWindowMinimumSize(win, 820, 560);
    SDL_Renderer* ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if (!ren) { fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError()); return 1; }

    // HiDPI (Retina): render at pixel density, lay out in points.
    int ww, wh, pw, ph;
    SDL_GetWindowSize(win, &ww, &wh);
    SDL_GetRendererOutputSize(ren, &pw, &ph);
    const float dpi = ww > 0 ? (float)pw / (float)ww : 1.0f;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    const float ui_scale = 1.0f;
    apply_style(ui_scale);
    load_fonts(dpi);
    ImGui_ImplSDL2_InitForSDLRenderer(win, ren);
    ImGui_ImplSDLRenderer2_Init(ren);

    bool quit = false;
    while (!quit) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            ImGui_ImplSDL2_ProcessEvent(&ev);
            if (ev.type == SDL_QUIT) quit = true;
            if (ev.type == SDL_DROPFILE && ev.drop.file) {
                std::lock_guard<std::mutex> lk(g_pick_mu);
                g_pending_imports.push_back(ev.drop.file);
                SDL_free(ev.drop.file);
            }
        }
        process_imports();
        reap_games();
        {
            // When a job ends, refresh the library (status / exe changed).
            static bool was_running = false;
            if (was_running && !g_job.running) { reload_library(); check_deps(); }
            was_running = g_job.running;
        }

        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();
        draw_ui(ui_scale);
        ImGui::Render();
        SDL_RenderSetScale(ren, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
        SDL_SetRenderDrawColor(ren, 0xB9, 0xBA, 0xB3, 255);
        SDL_RenderClear(ren);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData());
        SDL_RenderPresent(ren);
        if (!g_job.running && g_running.empty() && !g_picking) SDL_Delay(8);
    }

    if (g_job.running) cancel_job();
    if (g_job.th.joinable()) g_job.th.join();
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
