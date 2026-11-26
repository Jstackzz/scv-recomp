// The online menus' contract with the launcher (docs\PROTOCOL.md 3.8), see ui_link.h.
//
// ui.json: one UTF-8 JSON object, replaced whole by the launcher (3.2: a .tmp file moved over it), with `t` moving on at
// least once a second. The game reads it with every share flag (a reader never stops the launcher's replace), parses it
// with the small reader below (the SDK has no JSON library) and keeps the last good reading: a file caught half written
// or unreadable for a moment changes nothing. A `t` that has not moved for 5 s, or no file at all, is "the launcher
// does not answer".
//
// ask.txt: `ask n=<count> <verb> [key=value ...]`, written as a .tmp file moved into place once the launcher has taken
// the last one (renamed it to ask.txt.taken, as the game claims cmd.txt): the game keeps its own queue meanwhile.

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "sc5/ui_link.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>

#include <fmt/format.h>
#include <rex/logging.h>

#include "sc5/frame.h"

namespace sc5::uilink {
namespace {

// --- a small JSON reader: objects, arrays, strings (with \u escapes as UTF-8), numbers, true, false, null ---

struct Json {
  enum Kind { kNull, kBool, kNumber, kString, kArray, kObject } kind = kNull;
  bool b = false;
  double n = 0;
  std::string s;
  std::vector<Json> a;
  std::map<std::string, Json> o;

  const Json& operator[](const char* key) const {
    static const Json none;
    if (kind != kObject) return none;
    auto it = o.find(key);
    return it == o.end() ? none : it->second;
  }
  std::string Str() const { return kind == kString ? s : std::string(); }
  int64_t Int() const { return kind == kNumber ? int64_t(std::llround(n)) : 0; }
  bool Bool() const { return kind == kBool ? b : false; }
};

class Reader {
 public:
  explicit Reader(const std::string& text) : p_(text.data()), end_(text.data() + text.size()) {}

  bool Parse(Json& out) {
    if (!Value(out, 0)) return false;
    Space();
    return p_ == end_;
  }

 private:
  const char* p_;
  const char* end_;

  void Space() {
    while (p_ < end_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\r' || *p_ == '\n')) p_++;
  }
  bool Literal(const char* word) {
    const size_t n = std::strlen(word);
    if (size_t(end_ - p_) < n || std::memcmp(p_, word, n) != 0) return false;
    p_ += n;
    return true;
  }
  static void Utf8(uint32_t c, std::string& out) {
    if (c < 0x80) {
      out += char(c);
    } else if (c < 0x800) {
      out += char(0xC0 | (c >> 6));
      out += char(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
      out += char(0xE0 | (c >> 12));
      out += char(0x80 | ((c >> 6) & 0x3F));
      out += char(0x80 | (c & 0x3F));
    } else {
      out += char(0xF0 | (c >> 18));
      out += char(0x80 | ((c >> 12) & 0x3F));
      out += char(0x80 | ((c >> 6) & 0x3F));
      out += char(0x80 | (c & 0x3F));
    }
  }
  bool Hex4(uint32_t& v) {
    if (end_ - p_ < 4) return false;
    v = 0;
    for (int i = 0; i < 4; i++) {
      const char c = *p_++;
      v <<= 4;
      if (c >= '0' && c <= '9') v |= uint32_t(c - '0');
      else if (c >= 'a' && c <= 'f') v |= uint32_t(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') v |= uint32_t(c - 'A' + 10);
      else return false;
    }
    return true;
  }
  bool String(std::string& out) {
    if (p_ >= end_ || *p_ != '"') return false;
    p_++;
    while (p_ < end_ && *p_ != '"') {
      char c = *p_++;
      if (c != '\\') {
        out += c;
        continue;
      }
      if (p_ >= end_) return false;
      c = *p_++;
      switch (c) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
          uint32_t v = 0;
          if (!Hex4(v)) return false;
          if (v >= 0xD800 && v < 0xDC00 && end_ - p_ >= 6 && p_[0] == '\\' && p_[1] == 'u') {
            p_ += 2;
            uint32_t lo = 0;
            if (!Hex4(lo)) return false;
            v = 0x10000 + ((v - 0xD800) << 10) + (lo - 0xDC00);
          }
          Utf8(v, out);
          break;
        }
        default: return false;
      }
    }
    if (p_ >= end_) return false;
    p_++;
    return true;
  }
  bool Value(Json& v, int depth) {
    if (depth > 32) return false;
    Space();
    if (p_ >= end_) return false;
    const char c = *p_;
    if (c == '{') {
      p_++;
      v.kind = Json::kObject;
      Space();
      if (p_ < end_ && *p_ == '}') {
        p_++;
        return true;
      }
      for (;;) {
        Space();
        std::string key;
        if (!String(key)) return false;
        Space();
        if (p_ >= end_ || *p_++ != ':') return false;
        if (!Value(v.o[key], depth + 1)) return false;
        Space();
        if (p_ < end_ && *p_ == ',') {
          p_++;
          continue;
        }
        if (p_ < end_ && *p_ == '}') {
          p_++;
          return true;
        }
        return false;
      }
    }
    if (c == '[') {
      p_++;
      v.kind = Json::kArray;
      Space();
      if (p_ < end_ && *p_ == ']') {
        p_++;
        return true;
      }
      for (;;) {
        v.a.emplace_back();
        if (!Value(v.a.back(), depth + 1)) return false;
        Space();
        if (p_ < end_ && *p_ == ',') {
          p_++;
          continue;
        }
        if (p_ < end_ && *p_ == ']') {
          p_++;
          return true;
        }
        return false;
      }
    }
    if (c == '"') {
      v.kind = Json::kString;
      return String(v.s);
    }
    if (Literal("true")) {
      v.kind = Json::kBool;
      v.b = true;
      return true;
    }
    if (Literal("false")) {
      v.kind = Json::kBool;
      return true;
    }
    if (Literal("null")) return true;
    char* stop = nullptr;
    const std::string num(p_, size_t(std::min<ptrdiff_t>(end_ - p_, 40)));
    v.n = std::strtod(num.c_str(), &stop);
    if (stop == num.c_str()) return false;
    v.kind = Json::kNumber;
    p_ += stop - num.c_str();
    return true;
  }
};

// --- the files ---

bool g_on = false;
std::wstring g_ui, g_ask;
std::mutex g_lock;  // g_ui_now
Ui g_ui_now;
int64_t g_last_t = -1;
std::chrono::steady_clock::time_point g_t_moved;  // when `t` last moved on
std::string g_last_text;
uint64_t g_last_read = 0;
bool g_bad_logged = false;
std::deque<std::string> g_queue;  // asks not written yet
int g_count = 0;                  // asks written

std::wstring EnvW(const wchar_t* name) {
  wchar_t buf[4096];
  const DWORD n = GetEnvironmentVariableW(name, buf, DWORD(std::size(buf)));
  return n > 0 && n < std::size(buf) ? std::wstring(buf, n) : std::wstring();
}

bool ReadAll(const std::wstring& path, std::string& out) {
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  out.clear();
  char buf[16384];
  DWORD got = 0;
  bool ok = true;
  while (out.size() < (4u << 20)) {
    if (!ReadFile(h, buf, sizeof buf, &got, nullptr)) {
      ok = false;
      break;
    }
    if (!got) break;
    out.append(buf, got);
  }
  CloseHandle(h);
  if (out.size() >= 3 && uint8_t(out[0]) == 0xEF && uint8_t(out[1]) == 0xBB && uint8_t(out[2]) == 0xBF) out.erase(0, 3);
  return ok;
}

Ui FromJson(const Json& j) {
  Ui u;
  u.seen = true;
  u.t = j["t"].Int();
  const Json& acc = j["account"];
  u.account.state = acc["state"].Str();
  if (u.account.state.empty()) u.account.state = "signed_out";
  u.account.name = acc["name"].Str();
  u.account.rank = acc["rank"].Str();
  u.account.error = acc["error"].Str();
  u.account.elo = int(acc["elo"].Int());
  u.account.wins = int(acc["wins"].Int());
  u.account.losses = int(acc["losses"].Int());
  const Json& srv = j["server"];
  u.server.online = int(srv["online"].Int());
  u.server.in_matches = int(srv["in_matches"].Int());
  u.server.notice = srv["notice"].Str();
  u.side = j["side"].Str();
  if (u.side != "left" && u.side != "right") u.side = "off";
  const Json& q = j["queue"];
  u.queue.state = q["state"].Str();
  if (u.queue.state.empty()) u.queue.state = "idle";
  u.queue.kind = q["kind"].Str();
  u.queue.secs = int(q["secs"].Int());
  u.queue.accept_left = int(q["accept_left"].Int());
  u.queue.opponent = q["opponent"].Str();
  u.queue.opponent_rank = q["opponent_rank"].Str();
  u.queue.ping = int(q["ping"].Int());
  u.queue.text = q["text"].Str();
  const Json& se = j["series"];
  u.series.kind = se["kind"].Str();
  u.series.opponent = se["opponent"].Str();
  u.series.mine = int(se["mine"].Int());
  u.series.theirs = int(se["theirs"].Int());
  u.series.to = int(se["to"].Int());
  u.series.state = se["state"].Str();
  u.series.text = se["text"].Str();
  const Json& ls = j["lobbies"];
  u.lobbies_at = ls["at"].Int();
  for (const Json& r : ls["list"].a) {
    LobbyRow row;
    row.code = r["code"].Str();
    row.name = r["name"].Str();
    row.host = r["host"].Str();
    row.players = int(r["players"].Int());
    row.max = int(r["max"].Int());
    row.mode = r["mode"].Str();
    row.koth = r["koth"].Bool();
    row.locked = r["locked"].Bool();
    row.region = r["region"].Str();
    u.lobbies.push_back(std::move(row));
  }
  const Json& lb = j["lobby"];
  if (lb.kind == Json::kObject) {
    u.lobby.present = true;
    u.lobby.code = lb["code"].Str();
    u.lobby.name = lb["name"].Str();
    u.lobby.host = lb["host"].Str();
    u.lobby.mode = lb["mode"].Str();
    u.lobby.koth = lb["koth"].Bool();
    u.lobby.ready = lb["ready"].Bool();
    u.lobby.state = lb["state"].Str();
    u.lobby.text = lb["text"].Str();
    for (const Json& m : lb["members"].a) {
      Member mem;
      mem.name = m["name"].Str();
      mem.ready = m["ready"].Bool();
      mem.role = m["role"].Str();
      mem.wins = int(m["wins"].Int());
      u.lobby.members.push_back(std::move(mem));
    }
  }
  const Json& bd = j["leaderboard"];
  u.board_at = bd["at"].Int();
  for (const Json& r : bd["rows"].a) {
    BoardRow row;
    row.place = int(r["place"].Int());
    row.name = r["name"].Str();
    row.rank = r["rank"].Str();
    row.elo = int(r["elo"].Int());
    row.wins = int(r["wins"].Int());
    row.losses = int(r["losses"].Int());
    u.board.push_back(std::move(row));
  }
  const Json& msg = j["message"];
  u.message.id = msg["id"].Int();
  u.message.kind = msg["kind"].Str();
  u.message.text = msg["text"].Str();
  const Json& up = j["update"];
  u.update_version = up["version"].Str();
  u.update_text = up["text"].Str();
  return u;
}

void ReadUi() {
  std::string text;
  if (ReadAll(g_ui, text) && !text.empty() && text != g_last_text) {
    Json j;
    if (Reader(text).Parse(j) && j.kind == Json::kObject) {
      g_last_text = text;
      g_bad_logged = false;
      Ui u = FromJson(j);
      if (u.t != g_last_t) {
        g_last_t = u.t;
        g_t_moved = std::chrono::steady_clock::now();
      }
      std::lock_guard<std::mutex> lock(g_lock);
      const bool first = !g_ui_now.seen;
      const std::string was = g_ui_now.account.state + "/" + g_ui_now.queue.state;
      g_ui_now = std::move(u);
      const std::string now = g_ui_now.account.state + "/" + g_ui_now.queue.state;
      if (first || was != now)
        REXLOG_INFO("sc5 ui: ui.json: account {}, queue {}{}", g_ui_now.account.state, g_ui_now.queue.state,
                    g_ui_now.lobby.present ? ", in lobby " + g_ui_now.lobby.code : "");
    } else if (!g_bad_logged) {
      g_bad_logged = true;  // half written, or not JSON: the last good reading stands
      REXLOG_WARN("sc5 ui: ui.json could not be read as JSON ({} bytes): the last reading stands", text.size());
    }
  }
  const bool fresh =
      g_last_t >= 0 && std::chrono::steady_clock::now() - g_t_moved < std::chrono::seconds(5);
  static bool was_fresh = false;
  if (was_fresh != fresh) REXLOG_INFO("sc5 ui: the launcher {}", fresh ? "answers" : "does not answer (no new t for 5 s)");
  was_fresh = fresh;
  std::lock_guard<std::mutex> lock(g_lock);
  g_ui_now.fresh = fresh;
}

// the next ask once the launcher took the last one (ask.txt is gone: renamed to ask.txt.taken)
void WriteAsk() {
  if (g_queue.empty() || GetFileAttributesW(g_ask.c_str()) != INVALID_FILE_ATTRIBUTES) return;
  const std::string line = fmt::format("ask n={} {}", g_count + 1, g_queue.front());
  const std::wstring tmp = g_ask + L".tmp";
  FILE* f = _wfopen(tmp.c_str(), L"wb");
  if (!f) return;
  const std::string text = line + "\n";
  std::fwrite(text.data(), 1, text.size(), f);
  std::fclose(f);
  bool moved = false;
  for (int i = 0; i < 200 && !moved; i++) {
    moved = MoveFileExW(tmp.c_str(), g_ask.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!moved) Sleep(5);
  }
  if (!moved) return;
  g_count++;
  g_queue.pop_front();
  REXLOG_INFO("sc5 ui: {}", line);
}

}  // namespace

void Init() {
  const char* net = std::getenv("SC5_NET");
  const char* menus = std::getenv("SC5_MENUS");
  if (!net || std::strcmp(net, "1") != 0 || !menus || std::strcmp(menus, "1") != 0) return;
  const std::wstring dir = EnvW(L"SC5_SESSION_DIR");
  if (dir.empty()) {
    REXLOG_ERROR("sc5 ui: SC5_MENUS=1 without SC5_SESSION_DIR: no online menus");
    return;
  }
  g_ui = dir + L"\\ui.json";
  g_ask = dir + L"\\ask.txt";
  g_on = true;
  REXLOG_INFO("sc5 ui: the game's online menus: ui.json and ask.txt in the session folder");
}

bool On() { return g_on; }

void Poll(bool often) {
  if (!g_on) return;
  const uint64_t now = frame::Count();
  if (now >= g_last_read + (often ? 6 : 60) || now < g_last_read) {
    g_last_read = now;
    ReadUi();
  }
  WriteAsk();
}

Ui Snapshot() {
  std::lock_guard<std::mutex> lock(g_lock);
  return g_ui_now;
}

std::string Encode(const std::string& text) {
  std::string out;
  for (unsigned char c : text) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-')
      out += char(c);
    else
      out += fmt::format("%{:02X}", c);
  }
  return out;
}

void Ask(const std::string& verb, const std::vector<std::pair<std::string, std::string>>& keys) {
  if (!g_on) return;
  std::string line = verb;
  for (const auto& [k, v] : keys) line += " " + k + "=" + Encode(v);
  // a queue that does not grow without end while the launcher does not take them: the oldest go
  if (g_queue.size() >= 32) g_queue.pop_front();
  g_queue.push_back(line);
  REXLOG_INFO("sc5 ui: ask queued: {}", line);
  WriteAsk();
}

int AsksSent() { return g_count; }

}  // namespace sc5::uilink
