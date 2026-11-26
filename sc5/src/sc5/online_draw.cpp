// The online menus drawn over the main menu (online_menu.h), in the look of the game's own menus: Astral Home and
// Offline Play (pictures of both, 2026-11-26) have the screen's title top left on a long band with a bright flare at its
// left end, the entries as long blade-shaped bars (the chosen one wide and bright, its text dark; the others narrower,
// dimmer and set further in the further they are from it, the list moving so that the chosen one stays near the middle),
// the chosen entry's description under it, cards on the right (the player's license: a coloured band with the name,
// then its numbers), and the buttons' legend at the bottom right. Here the same, larger: the owner sees poorly, so every
// text is at least a 26th of the screen's height, light on dark or dark on light, and nothing is said by colour alone
// (a button's letter is in its circle, a lobby member is READY or NOT READY in words beside its tick or box, the
// player's own leaderboard row says YOU).
//
// Fonts: Palatino Linotype Bold for the titles and entries (the game's are a classical serif), Segoe UI Semibold for
// the rest, both Windows' own; Georgia Bold and Segoe UI Bold where those are missing. Drawn with ImGui's draw list on
// the UI thread from the menus' View (a copy the main thread makes every frame); it reads no input and changes nothing.

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <string>

#include <imgui.h>
#include <imgui_internal.h>
#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>

#include "sc5/online_menu.h"

namespace sc5::online {
namespace {

ImFont* g_serif = nullptr;
ImFont* g_sans = nullptr;

ImFont* Serif() { return g_serif ? g_serif : ImGui::GetFont(); }
ImFont* Sans() { return g_sans ? g_sans : Serif(); }

// the screen: a 16:9 frame in the window, measured in the console's 1280x720
struct Frame {
  float x = 0, y = 0, s = 1;
  ImVec2 P(float px, float py) const { return ImVec2(x + px * s, y + py * s); }
  float L(float v) const { return v * s; }
};

ImU32 Rgba(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }
ImU32 Fade(ImU32 c, float f) {
  const int a = int(((c >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(f, 0.0f, 1.0f));
  return (c & ~IM_COL32_A_MASK) | (ImU32(a) << IM_COL32_A_SHIFT);
}

const ImU32 kInk = Rgba(8, 14, 34);           // text on a bright bar
const ImU32 kWhite = Rgba(245, 248, 255);
const ImU32 kPale = Rgba(214, 226, 246);      // the other entries' text
const ImU32 kGold = Rgba(255, 214, 110);
const ImU32 kDim = Rgba(150, 160, 182);
const ImU32 kShadow = Rgba(0, 0, 0, 230);

float Width(ImFont* f, float px, const std::string& t) { return f->CalcTextSizeA(px, FLT_MAX, 0.0f, t.c_str()).x; }

// text with a dark outline (readable over anything)
void Outlined(ImDrawList* dl, ImFont* f, float px, ImVec2 at, ImU32 col, const std::string& t, float o) {
  static const float kDirs[8][2] = {{-1, -1}, {0, -1}, {1, -1}, {-1, 0}, {1, 0}, {-1, 1}, {0, 1}, {1, 1}};
  for (const auto& d : kDirs) dl->AddText(f, px, ImVec2(at.x + d[0] * o, at.y + d[1] * o), Fade(kShadow, ((col >> 24) & 0xFF) / 255.0f), t.c_str());
  dl->AddText(f, px, at, col, t.c_str());
}

// the game's titles: silver letters, brighter in their upper half, on a dark outline
void Metal(ImDrawList* dl, ImFont* f, float px, ImVec2 at, const std::string& t, float o, bool gold = false) {
  Outlined(dl, f, px, at, gold ? Rgba(214, 160, 60) : Rgba(170, 186, 212), t, o);
  const float w = Width(f, px, t);
  dl->PushClipRect(ImVec2(at.x - o, at.y), ImVec2(at.x + w + o, at.y + px * 0.56f), true);
  dl->AddText(f, px, at, gold ? Rgba(255, 236, 170) : Rgba(250, 252, 255), t.c_str());
  dl->PopClipRect();
}

// a line of text shrunk to fit `room` (never below 70%; cut with "..." beyond)
std::string Fit(ImFont* f, float& px, const std::string& t, float room) {
  const float w = Width(f, px, t);
  if (w <= room) return t;
  if (w * 0.7f <= room) {
    px *= room / w;
    return t;
  }
  px *= 0.7f;
  std::string s = t;
  while (s.size() > 1 && Width(f, px, s + "...") > room) s.pop_back();
  return s + "...";
}

// a soft glow: circles fading out
void Glow(ImDrawList* dl, ImVec2 c, float r, ImU32 col, int rings = 14) {
  for (int i = rings; i >= 1; i--) {
    const float k = float(i) / rings;
    dl->AddCircleFilled(c, r * k, Fade(col, (1.0f - k) * 0.22f + 0.02f), 48);
  }
}

// the blade: a long bar with pointed ends, a vertical gradient
void Blade(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 top, ImU32 bottom) {
  const float h = b.y - a.y, tip = h * 0.5f, mid = (a.y + b.y) * 0.5f;
  const ImVec2 pts[6] = {{a.x, mid}, {a.x + tip, a.y}, {b.x - tip, a.y}, {b.x, mid}, {b.x - tip, b.y}, {a.x + tip, b.y}};
  const int v0 = dl->VtxBuffer.Size;
  dl->AddConvexPolyFilled(pts, 6, top);
  ImGui::ShadeVertsLinearColorGradientKeepAlpha(dl, v0, dl->VtxBuffer.Size, ImVec2(a.x, a.y), ImVec2(a.x, b.y), top, bottom);
}
void BladeEdge(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, float t) {
  const float h = b.y - a.y, tip = h * 0.5f, mid = (a.y + b.y) * 0.5f;
  const ImVec2 pts[6] = {{a.x, mid}, {a.x + tip, a.y}, {b.x - tip, a.y}, {b.x, mid}, {b.x - tip, b.y}, {a.x + tip, b.y}};
  dl->AddPolyline(pts, 6, col, ImDrawFlags_Closed, t);
}

// a panel in the game's way: dark glass, a light frame, small_px bright corners
void Panel(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 edge, float t, float alpha = 0.86f) {
  dl->AddRectFilledMultiColor(a, b, Rgba(16, 30, 72, int(255 * alpha)), Rgba(16, 30, 72, int(255 * alpha)),
                              Rgba(4, 10, 30, int(255 * alpha)), Rgba(4, 10, 30, int(255 * alpha)));
  dl->AddRect(a, b, edge, 0, 0, t);
  const float c = t * 7;
  for (int i = 0; i < 4; i++) {
    const float x = i & 1 ? b.x : a.x, y = i & 2 ? b.y : a.y, dx = i & 1 ? -c : c, dy = i & 2 ? -c : c;
    dl->AddLine(ImVec2(x, y), ImVec2(x + dx, y), kWhite, t * 1.6f);
    dl->AddLine(ImVec2(x, y), ImVec2(x, y + dy), kWhite, t * 1.6f);
  }
}

// a button of the legend: its letter in a circle of the pad's colour (the letter says it, not the colour), or the d-pad
float ButtonIcon(ImDrawList* dl, const Frame& f, ImVec2 c, const std::string& b) {
  const float r = f.L(17);
  if (b == "UD" || b == "LR") {
    const float arm = r * 0.42f;
    dl->AddCircleFilled(c, r, Rgba(40, 44, 56), 32);
    dl->AddCircle(c, r, Rgba(200, 206, 220), 32, f.L(2));
    dl->AddRectFilled(ImVec2(c.x - arm, c.y - r * 0.8f), ImVec2(c.x + arm, c.y + r * 0.8f), Rgba(120, 126, 140));
    dl->AddRectFilled(ImVec2(c.x - r * 0.8f, c.y - arm), ImVec2(c.x + r * 0.8f, c.y + arm), Rgba(120, 126, 140));
    const bool ud = b == "UD";
    const float k = r * 0.72f, w = r * 0.36f;
    for (int s = -1; s <= 1; s += 2) {
      if (ud)
        dl->AddTriangleFilled(ImVec2(c.x, c.y + s * k), ImVec2(c.x - w, c.y + s * (k - w)), ImVec2(c.x + w, c.y + s * (k - w)), kWhite);
      else
        dl->AddTriangleFilled(ImVec2(c.x + s * k, c.y), ImVec2(c.x + s * (k - w), c.y - w), ImVec2(c.x + s * (k - w), c.y + w), kWhite);
    }
    return r;
  }
  ImU32 col = Rgba(60, 60, 70);
  if (b == "A") col = Rgba(46, 160, 56);
  if (b == "B") col = Rgba(206, 44, 44);
  if (b == "X") col = Rgba(40, 96, 214);
  if (b == "Y") col = Rgba(226, 168, 24);
  dl->AddCircleFilled(ImVec2(c.x, c.y + f.L(1.5f)), r, Rgba(0, 0, 0, 160), 32);
  dl->AddCircleFilled(c, r, col, 32);
  dl->AddCircle(c, r, Rgba(255, 255, 255, 200), 32, f.L(1.5f));
  const float px = f.L(25);
  const float w = Width(Sans(), px, b);
  Outlined(dl, Sans(), px, ImVec2(c.x - w * 0.5f, c.y - px * 0.55f), kWhite, b, f.L(1.2f));
  return r;
}

// a tick (ready) or an empty box (not ready), with the word beside it said by the caller
void Mark(ImDrawList* dl, const Frame& f, ImVec2 c, bool tick) {
  const float r = f.L(11);
  dl->AddRect(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), kWhite, 0, 0, f.L(2.5f));
  if (tick) {
    dl->AddRectFilled(ImVec2(c.x - r + f.L(2), c.y - r + f.L(2)), ImVec2(c.x + r - f.L(2), c.y + r - f.L(2)), Rgba(40, 150, 70));
    const ImVec2 pts[3] = {{c.x - r * 0.6f, c.y}, {c.x - r * 0.15f, c.y + r * 0.5f}, {c.x + r * 0.65f, c.y - r * 0.55f}};
    dl->AddPolyline(pts, 3, kWhite, 0, f.L(3.5f));
  }
}

class MenuDialog final : public rex::ui::ImGuiDialog {
 public:
  explicit MenuDialog(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}
  bool RepaintsContinuously() const override { return false; }

 protected:
  void OnDraw(ImGuiIO& io) override {
    const View v = ViewNow();
    if (!v.shown || io.DisplaySize.x < 64 || io.DisplaySize.y < 64) return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const float W = io.DisplaySize.x, H = io.DisplaySize.y;
    Frame f;
    f.s = std::min(W / 1280.0f, H / 720.0f);
    f.x = (W - 1280.0f * f.s) * 0.5f;
    f.y = (H - 720.0f * f.s) * 0.5f;
    const float t = float(ImGui::GetTime());
    Background(dl, f, W, H, t);
    TitleBand(dl, f, v.title, t);
    float top = 118;
    if (!v.banner_head.empty()) top = Banner(dl, f, v, top) + 12;
    if (v.stage) top = Stage(dl, f, v, top, t) + 14;
    List(dl, f, v, top, t);
    Cards(dl, f, v);
    Legend(dl, f, v.legend);
    if (v.popup) Popup(dl, f, W, H, v);
  }

 private:
  // The backdrop: Astral Home's own (its blue swirl around a dark orb) is busy with its own entries and cards, which
  // must not show through ours (even a trace of them was noise behind our text), so it is covered, and an astral sky of
  // ours drawn in its place: deep blue,
  // a glowing orb with orbits turning slowly where the game's is, a few streaks of light
  static void Background(ImDrawList* dl, const Frame& f, float W, float H, float t) {
    dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(W, H), Rgba(10, 20, 58), Rgba(8, 16, 50), Rgba(2, 5, 18),
                                Rgba(3, 7, 24));
    const ImVec2 orb = f.P(1000, 330);
    const float pulse = 0.92f + 0.08f * std::sin(t * 1.3f);
    Glow(dl, orb, f.L(380) * pulse, Rgba(30, 90, 220), 20);
    Glow(dl, orb, f.L(150), Rgba(110, 180, 255), 12);
    for (int i = 0; i < 4; i++) {
      const float rx = f.L(170 + i * 70), ry = rx * (0.28f + 0.05f * i);
      const float rot = 0.35f + i * 0.5f + t * (0.05f + 0.02f * i) * (i & 1 ? -1.0f : 1.0f);
      dl->AddEllipse(orb, ImVec2(rx, ry), Rgba(150, 200, 255, 46 - i * 7), rot, 96, f.L(2.0f));
    }
    dl->AddCircleFilled(orb, f.L(62), Rgba(2, 4, 12, 235), 64);
    dl->AddCircle(orb, f.L(62), Rgba(170, 215, 255, 160), 64, f.L(2.5f));
    for (int i = 0; i < 5; i++) {
      const float k = std::fmod(t * 0.03f + i * 0.21f, 1.0f);
      const ImVec2 a = f.P(560 + 700 * k, 0 - 40), b = f.P(260 + 700 * k, 760);
      dl->AddLine(a, b, Rgba(120, 180, 255, int(18 * std::sin(k * 3.14159f))), f.L(26));
    }
  }

  static void TitleBand(ImDrawList* dl, const Frame& f, const std::string& title, float t) {
    const ImVec2 a = f.P(0, 40), b = f.P(820, 102);
    dl->AddRectFilledMultiColor(a, b, Rgba(36, 70, 160, 235), Rgba(10, 24, 70, 0), Rgba(10, 24, 70, 0),
                                Rgba(20, 40, 110, 235));
    dl->AddLine(a, ImVec2(b.x * 0.9f, a.y), Rgba(170, 210, 255, 200), f.L(2));
    dl->AddLine(ImVec2(a.x, b.y), ImVec2(b.x * 0.75f, b.y), Rgba(170, 210, 255, 160), f.L(2));
    // the flare at its left end
    const float pulse = 0.9f + 0.1f * std::sin(t * 2.0f);
    Glow(dl, f.P(34, 71), f.L(70) * pulse, Rgba(150, 210, 255), 12);
    dl->AddCircleFilled(f.P(34, 71), f.L(11), Rgba(255, 255, 255, 235), 24);
    dl->AddLine(f.P(0, 71), f.P(120, 71), Rgba(255, 255, 255, 150), f.L(2));
    float px = f.L(50);
    const std::string text = Fit(Serif(), px, title, f.L(680));
    Metal(dl, Serif(), px, ImVec2(f.P(100, 0).x, f.P(0, 71).y - px * 0.55f), text, f.L(2.2f));
  }

  // a state the player must know: a heading and a line, with a sign ("!" in a circle, or "i")
  static float Banner(ImDrawList* dl, const Frame& f, const View& v, float y) {
    const float x0 = 40, x1 = 810, pad = 14;
    const float hp = f.L(30), tp = f.L(27);
    const float text_w = f.L(x1 - x0 - 2 * pad - 60);
    const float th = Sans()->CalcTextSizeA(tp, FLT_MAX, text_w, v.banner_text.c_str()).y;
    const float h = pad + 34 + th / f.s + pad;
    const ImVec2 a = f.P(x0, y), b = f.P(x1, y + h);
    const ImU32 edge = v.banner_error ? Rgba(255, 120, 110) : Rgba(140, 190, 255);
    dl->AddRectFilledMultiColor(a, b, v.banner_error ? Rgba(110, 18, 24, 235) : Rgba(20, 50, 120, 235),
                                v.banner_error ? Rgba(60, 10, 20, 225) : Rgba(10, 26, 70, 225),
                                v.banner_error ? Rgba(60, 10, 20, 225) : Rgba(10, 26, 70, 225),
                                v.banner_error ? Rgba(110, 18, 24, 235) : Rgba(20, 50, 120, 235));
    dl->AddRect(a, b, edge, 0, 0, f.L(3));
    const ImVec2 c = f.P(x0 + pad + 22, y + pad + 20);
    dl->AddCircleFilled(c, f.L(20), kWhite, 32);
    const std::string sign = v.banner_error ? "!" : "i";
    const float sp = f.L(32);
    dl->AddText(Serif(), sp, ImVec2(c.x - Width(Serif(), sp, sign) * 0.5f, c.y - sp * 0.56f),
                v.banner_error ? Rgba(150, 20, 20) : Rgba(20, 60, 150), sign.c_str());
    Outlined(dl, Serif(), hp, f.P(x0 + pad + 56, y + pad + 2), kWhite, v.banner_head, f.L(1.5f));
    dl->AddText(Sans(), tp, f.P(x0 + pad + 56, y + pad + 38), kWhite, v.banner_text.c_str(), nullptr, text_w);
    return y + h;
  }

  // the search, a match found, the series: a framed panel above the list
  static float Stage(ImDrawList* dl, const Frame& f, const View& v, float y, float t) {
    const float x0 = 40, x1 = 810;
    const float hp = f.L(36), bp = f.L(60), sp = f.L(30), lp = f.L(27);
    const float room = f.L(x1 - x0 - 40);
    float h = 20 + 44 + (v.stage_big.empty() ? 0 : 70) + (v.stage_sub.empty() ? 0 : 40);
    for (const std::string& l : v.stage_lines) h += Sans()->CalcTextSizeA(lp, FLT_MAX, room, l.c_str()).y / f.s + 6;
    h += 16;
    const ImVec2 a = f.P(x0, y), b = f.P(x1, y + h);
    const bool found = v.stage_head == "MATCH FOUND";
    const float pulse = 0.5f + 0.5f * std::sin(t * (found ? 6.0f : 2.5f));
    if (found) {
      for (int i = 6; i >= 1; i--)
        dl->AddRect(ImVec2(a.x - f.L(i * 2.0f), a.y - f.L(i * 2.0f)), ImVec2(b.x + f.L(i * 2.0f), b.y + f.L(i * 2.0f)),
                    Fade(kGold, (0.10f + 0.10f * pulse) * (7 - i) / 6.0f), 0, 0, f.L(2.5f));
    }
    Panel(dl, a, b, found ? kGold : Rgba(140, 180, 240), f.L(2.5f), 0.9f);
    float cy = y + 18;
    const float cx = (x0 + x1) * 0.5f;
    {
      float px = hp;
      const std::string s = Fit(Serif(), px, v.stage_head, room);
      Metal(dl, Serif(), px, ImVec2(f.P(cx, 0).x - Width(Serif(), px, s) * 0.5f, f.P(0, cy).y), s, f.L(2), true);
      cy += 44;
    }
    if (!v.stage_big.empty()) {
      float px = bp;
      const std::string s = Fit(Serif(), px, v.stage_big, room);
      Outlined(dl, Serif(), px, ImVec2(f.P(cx, 0).x - Width(Serif(), px, s) * 0.5f, f.P(0, cy).y), kWhite, s, f.L(2.5f));
      cy += 70;
    }
    if (!v.stage_sub.empty()) {
      float px = sp;
      const std::string s = Fit(Sans(), px, v.stage_sub, room);
      Outlined(dl, Sans(), px, ImVec2(f.P(cx, 0).x - Width(Sans(), px, s) * 0.5f, f.P(0, cy).y), found ? kGold : kPale, s,
               f.L(1.5f));
      cy += 40;
    }
    for (const std::string& l : v.stage_lines) {
      const ImVec2 sz = Sans()->CalcTextSizeA(lp, FLT_MAX, room, l.c_str());
      const float x = sz.x < room ? f.P(cx, 0).x - sz.x * 0.5f : f.P(x0 + 20, 0).x;
      dl->AddText(Sans(), lp, ImVec2(x, f.P(0, cy).y), kWhite, l.c_str(), nullptr, room);
      cy += sz.y / f.s + 6;
    }
    return y + h;
  }

  // The entries: the chosen one at a fixed height where it can be (the list moves under it, as the game's), the
  // description under it, the others above and below, set further in the further they are
  static void List(ImDrawList* dl, const Frame& f, const View& v, float top, float t) {
    const int n = int(v.rows.size());
    if (!n) return;
    const float bottom = 628, pitch = 56, sel_h = 60, bar_h = 44, gap = 10;
    const float desc_px = f.L(29), desc_room = f.L(720);
    const float desc_h =
        v.desc.empty() ? 8 : Sans()->CalcTextSizeA(desc_px, FLT_MAX, desc_room, v.desc.c_str()).y / f.s + 26;
    const int c = std::clamp(v.cursor, 0, n - 1);
    const float need_above = c * pitch + gap, need_below = sel_h + desc_h + (n - 1 - c) * pitch;
    const float pref = std::max(top + std::min(c, 3) * pitch + gap, 300.0f);
    float ys;
    if (top + need_above <= bottom - need_below) ys = std::clamp(pref, top + need_above, bottom - need_below);
    else ys = std::clamp(pref, top + std::min(c, 2) * pitch + gap, std::max(top + gap, bottom - sel_h - desc_h - pitch));
    bool cut_above = false, cut_below = false;
    for (int i = 0; i < n; i++) {
      if (i == c) continue;
      const int d = std::abs(i - c);
      const float y = i < c ? ys - gap - (c - i) * pitch + (pitch - bar_h) : ys + sel_h + desc_h + (i - c - 1) * pitch;
      if (y < top - 2) {
        cut_above = true;
        continue;
      }
      if (y + bar_h > bottom + 30) {
        cut_below = true;
        continue;
      }
      const float x0 = 64 + 26 * std::min(d, 5), x1 = 730 - 10 * std::min(d, 5);
      Bar(dl, f, v.rows[size_t(i)], x0, y, x1, y + bar_h, false, t);
    }
    Bar(dl, f, v.rows[size_t(c)], 22, ys, 800, ys + sel_h, true, t);
    if (!v.desc.empty()) {
      // white on the dark backdrop, a shadow under it
      const ImVec2 at = f.P(66, ys + sel_h + 8);
      dl->AddText(Sans(), desc_px, ImVec2(at.x + f.L(2), at.y + f.L(2)), kShadow, v.desc.c_str(), nullptr, desc_room);
      dl->AddText(Sans(), desc_px, at, kWhite, v.desc.c_str(), nullptr, desc_room);
    }
    // more entries than the screen holds: a pointer where they are
    const auto arrow = [&](float y, bool up) {
      const ImVec2 c0 = f.P(400, y);
      const float w = f.L(14), h = f.L(10) * (up ? -1 : 1);
      dl->AddTriangleFilled(ImVec2(c0.x - w, c0.y - h * 0.5f), ImVec2(c0.x + w, c0.y - h * 0.5f), ImVec2(c0.x, c0.y + h * 0.5f), kGold);
    };
    if (cut_above) arrow(top - 4, true);
    if (cut_below) arrow(bottom + 40, false);
  }

  static void Bar(ImDrawList* dl, const Frame& f, const Row& r, float x0, float y0, float x1, float y1, bool chosen, float t) {
    const ImVec2 a = f.P(x0, y0), b = f.P(x1, y1);
    const float h = b.y - a.y;
    if (chosen) {
      const float pulse = 0.5f + 0.5f * std::sin(t * 3.0f);
      for (int i = 5; i >= 1; i--)
        BladeEdge(dl, ImVec2(a.x - f.L(i * 2.2f), a.y - f.L(i * 1.6f)), ImVec2(b.x + f.L(i * 2.2f), b.y + f.L(i * 1.6f)),
                  Rgba(110, 190, 255, int((40 + 30 * pulse) * (6 - i) / 5)), f.L(3));
      if (r.enabled) Blade(dl, a, b, Rgba(236, 246, 255), Rgba(70, 128, 226));
      else Blade(dl, a, b, Rgba(196, 200, 210), Rgba(100, 106, 122));
      // the light running along it
      const float run = std::fmod(t * 0.45f, 1.6f) - 0.3f;
      const float sx = a.x + (b.x - a.x) * run;
      dl->PushClipRect(ImVec2(a.x + h * 0.5f, a.y), ImVec2(b.x - h * 0.5f, b.y), true);
      dl->AddRectFilledMultiColor(ImVec2(sx - f.L(90), a.y), ImVec2(sx, b.y), Rgba(255, 255, 255, 0), Rgba(255, 255, 255, 120),
                                  Rgba(255, 255, 255, 120), Rgba(255, 255, 255, 0));
      dl->AddRectFilledMultiColor(ImVec2(sx, a.y), ImVec2(sx + f.L(90), b.y), Rgba(255, 255, 255, 120), Rgba(255, 255, 255, 0),
                                  Rgba(255, 255, 255, 0), Rgba(255, 255, 255, 120));
      dl->AddLine(ImVec2(a.x, a.y + h * 0.3f), ImVec2(b.x, a.y + h * 0.3f), Rgba(255, 255, 255, 110), f.L(2));
      dl->PopClipRect();
      BladeEdge(dl, a, b, r.mine ? kGold : Rgba(255, 255, 255, 230), f.L(r.mine ? 4 : 2.5f));
      // the hilt at its left end, as the game's chosen entry has: a crossguard and a gem
      const float gx = a.x + h * 0.95f, mid = (a.y + b.y) * 0.5f, g = h * 0.22f;
      dl->AddLine(ImVec2(gx, a.y - h * 0.10f), ImVec2(gx, b.y + h * 0.10f), Rgba(235, 245, 255, 230), f.L(4));
      const ImVec2 gem[4] = {{gx - g, mid}, {gx, mid - g}, {gx + g, mid}, {gx, mid + g}};
      dl->AddConvexPolyFilled(gem, 4, Rgba(40, 110, 230));
      dl->AddPolyline(gem, 4, kWhite, ImDrawFlags_Closed, f.L(2));
    } else {
      Blade(dl, a, b, Rgba(40, 70, 146, 225), Rgba(8, 18, 52, 225));
      BladeEdge(dl, a, b, r.mine ? kGold : Rgba(130, 170, 236, 230), f.L(r.mine ? 3.5f : 2));
      dl->AddLine(ImVec2(a.x + h * 0.5f, a.y + f.L(3)), ImVec2(b.x - h * 0.5f, a.y + f.L(3)), Rgba(200, 225, 255, 90), f.L(1.5f));
    }
    const ImU32 col = chosen ? (r.enabled ? kInk : Rgba(40, 44, 56)) : (r.enabled ? kPale : kDim);
    const float label_px0 = f.L(chosen ? 40 : 35);
    const float tx = a.x + f.L(chosen ? 88 : 40);
    // the right end: a setting's value between arrows, the row's detail, NOT AVAILABLE, YOU
    float right = b.x - h * 0.6f;
    const float small_px = f.L(chosen ? 30 : 27);
    std::string tail;
    if (!r.detail.empty()) tail = r.detail;
    else if (!r.enabled) tail = "NOT AVAILABLE";
    if (!r.value.empty()) {
      const float vp = f.L(chosen ? 36 : 32);
      const float vw = Width(Serif(), vp, r.value);
      const float aw = f.L(10), mid = (a.y + b.y) * 0.5f;
      float vx = right - vw;
      if (r.arrows) {
        const float ax = right - aw;
        dl->AddTriangleFilled(ImVec2(ax, mid - aw), ImVec2(ax, mid + aw), ImVec2(ax + aw, mid), col);
        vx = ax - f.L(10) - vw;
        const float bx = vx - f.L(10);
        dl->AddTriangleFilled(ImVec2(bx, mid - aw), ImVec2(bx, mid + aw), ImVec2(bx - aw, mid), col);
        right = bx - aw - f.L(18);
      } else {
        right = vx - f.L(18);
      }
      if (chosen) dl->AddText(Serif(), vp, ImVec2(vx, mid - vp * 0.56f), col, r.value.c_str());
      else Outlined(dl, Serif(), vp, ImVec2(vx, mid - vp * 0.56f), col, r.value, f.L(1.5f));
    }
    if (!tail.empty()) {
      const float tw = Width(Sans(), small_px, tail);
      const ImVec2 at(right - tw, (a.y + b.y) * 0.5f - small_px * 0.6f);
      if (chosen) dl->AddText(Sans(), small_px, at, col, tail.c_str());
      else Outlined(dl, Sans(), small_px, at, col, tail, f.L(1.2f));
      right -= tw + f.L(18);
    }
    if (r.mine) {
      const float yp = f.L(26), yw = Width(Sans(), yp, "YOU") + f.L(16);
      const ImVec2 ya(right - yw, (a.y + b.y) * 0.5f - yp * 0.62f), yb(right, (a.y + b.y) * 0.5f + yp * 0.62f);
      dl->AddRectFilled(ya, yb, kGold, f.L(4));
      dl->AddText(Sans(), yp, ImVec2(ya.x + f.L(8), ya.y + yp * 0.08f), kInk, "YOU");
      right = ya.x - f.L(14);
    }
    float px = label_px0;
    const std::string label = Fit(Serif(), px, r.label, std::max(f.L(80), right - tx));
    const ImVec2 at(tx, (a.y + b.y) * 0.5f - px * 0.56f);
    if (chosen) dl->AddText(Serif(), px, at, col, label.c_str());
    else Outlined(dl, Serif(), px, at, col, label, f.L(1.8f));
  }

  // the cards on the right: the player's license (name on a red band as the game's user card), its numbers; a second
  // card (a lobby's members); the server's notice
  static void Cards(ImDrawList* dl, const Frame& f, const View& v) {
    const float x0 = 846, x1 = 1236;
    float y = 40;
    const float name_px = f.L(34), row_px = f.L(28);
    {
      const float h = 64 + 36 + v.account.size() * 36.0f + 14;
      Panel(dl, f.P(x0, y), f.P(x1, y + h), Rgba(230, 180, 180, 200), f.L(2));
      dl->AddRectFilledMultiColor(f.P(x0 + 3, y + 3), f.P(x1 - 3, y + 56), Rgba(170, 46, 52, 240), Rgba(110, 30, 44, 200),
                                  Rgba(70, 16, 30, 200), Rgba(130, 34, 44, 240));
      float px = name_px;
      const std::string name = Fit(Sans(), px, v.name, f.L(x1 - x0 - 30));
      Outlined(dl, Sans(), px, f.P(x0 + 14, y + 8), kWhite, name, f.L(1.5f));
      float rp = row_px;
      const std::string rank = Fit(Serif(), rp, v.rank, f.L(x1 - x0 - 30));
      Outlined(dl, Serif(), rp, ImVec2(f.P(x1 - 14, 0).x - Width(Serif(), rp, rank), f.P(0, y + 64).y), kGold, rank, f.L(1.5f));
      float ry = y + 100;
      for (const Line& l : v.account) {
        Outlined(dl, Sans(), row_px, f.P(x0 + 16, ry), kPale, l.name, f.L(1.2f));
        Outlined(dl, Sans(), row_px, ImVec2(f.P(x1 - 16, 0).x - Width(Sans(), row_px, l.value), f.P(0, ry).y), kWhite, l.value,
                 f.L(1.2f));
        dl->AddLine(f.P(x0 + 12, ry + 34), f.P(x1 - 12, ry + 34), Rgba(140, 170, 230, 70), f.L(1));
        ry += 36;
      }
      y += h + 12;
    }
    if (!v.card_head.empty() || !v.card.empty()) {
      const float h = 50 + std::max<size_t>(v.card.size(), 1) * 38.0f + 12;
      Panel(dl, f.P(x0, y), f.P(x1, y + h), Rgba(150, 190, 250, 220), f.L(2));
      dl->AddRectFilledMultiColor(f.P(x0 + 3, y + 3), f.P(x1 - 3, y + 44), Rgba(40, 80, 170, 240), Rgba(20, 40, 100, 200),
                                  Rgba(14, 30, 80, 200), Rgba(30, 60, 140, 240));
      float hp = f.L(28);
      const std::string head = Fit(Sans(), hp, v.card_head, f.L(x1 - x0 - 28));
      Outlined(dl, Sans(), hp, f.P(x0 + 14, y + 8), kWhite, head, f.L(1.2f));
      float ry = y + 52;
      for (const Line& l : v.card) {
        float nx = x0 + 16;
        if (l.mark) {
          Mark(dl, f, f.P(x0 + 28, ry + 16), l.mark == 1);
          nx = x0 + 48;
        }
        float vp = f.L(27);
        const float vw = Width(Sans(), vp, l.value);
        float np = row_px;
        const std::string nm = Fit(Sans(), np, l.name, f.L(x1 - nx - 24) - vw);
        Outlined(dl, Sans(), np, f.P(nx, ry), l.strong ? kGold : kWhite, nm, f.L(1.2f));
        Outlined(dl, Sans(), vp, ImVec2(f.P(x1 - 14, 0).x - vw, f.P(0, ry + 3).y), l.mark == 1 ? Rgba(170, 255, 180) : kPale,
                 l.value, f.L(1.2f));
        ry += 38;
      }
      y += h + 12;
    }
    // the update waiting and the server's notice: each a gold heading and its text
    if (!v.news.empty() && y < 560) {
      const float hp = f.L(26), np = f.L(27), room = f.L(x1 - x0 - 28);
      float h = 12;
      for (const auto& [head, text] : v.news) h += 34 + Sans()->CalcTextSizeA(np, FLT_MAX, room, text.c_str()).y / f.s + 8;
      h = std::min(h, 630.0f - y);
      Panel(dl, f.P(x0, y), f.P(x1, y + h), kGold, f.L(2));
      dl->PushClipRect(f.P(x0, y), f.P(x1, y + h), true);
      float ny = y + 8;
      for (const auto& [head, text] : v.news) {
        Metal(dl, Serif(), hp, f.P(x0 + 14, ny), head, f.L(1.5f), true);
        ny += 34;
        dl->AddText(Sans(), np, f.P(x0 + 14, ny), kWhite, text.c_str(), nullptr, room);
        ny += Sans()->CalcTextSizeA(np, FLT_MAX, room, text.c_str()).y / f.s + 8;
      }
      dl->PopClipRect();
    }
  }

  // the buttons at the bottom right, as the game's own legend
  static void Legend(ImDrawList* dl, const Frame& f, const std::vector<std::pair<std::string, std::string>>& items) {
    const float px = f.L(28), y = 668;
    float total = 0;
    for (const auto& [b, what] : items) total += 34 + 8 + Width(Sans(), px, what) / f.s + 30;
    float x = std::max(40.0f, 1244 - total);
    dl->AddRectFilledMultiColor(f.P(x - 30, y - 26), f.P(1280, y + 26), Rgba(0, 0, 0, 0), Rgba(0, 0, 0, 150),
                                Rgba(0, 0, 0, 150), Rgba(0, 0, 0, 0));
    for (const auto& [b, what] : items) {
      ButtonIcon(dl, f, f.P(x + 17, y), b);
      x += 34 + 8;
      Outlined(dl, Sans(), px, f.P(x, y - 19), kWhite, what, f.L(1.5f));
      x += Width(Sans(), px, what) / f.s + 30;
    }
  }

  // a message the player closes with A: in the middle, over everything, the rest darkened
  static void Popup(ImDrawList* dl, const Frame& f, float W, float H, const View& v) {
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), Rgba(0, 0, 0, 150));
    const float x0 = 250, x1 = 1030, tp = f.L(30), room = f.L(x1 - x0 - 60);
    const float th = Sans()->CalcTextSizeA(tp, FLT_MAX, room, v.popup_text.c_str()).y / f.s;
    const float h = 30 + 50 + th + 30 + 50;
    const float y = 360 - h * 0.5f;
    const ImU32 edge = v.popup_error ? Rgba(255, 110, 100) : kGold;
    for (int i = 4; i >= 1; i--)
      dl->AddRect(f.P(x0 - i * 3.0f, y - i * 3.0f), f.P(x1 + i * 3.0f, y + h + i * 3.0f), Fade(edge, 0.12f * (5 - i)), 0, 0,
                  f.L(3));
    Panel(dl, f.P(x0, y), f.P(x1, y + h), edge, f.L(3.5f), 0.97f);
    const ImVec2 c = f.P(x0 + 50, y + 52);
    dl->AddCircleFilled(c, f.L(22), edge, 32);
    const float sp = f.L(34);
    const std::string sign = v.popup_error ? "!" : "i";
    dl->AddText(Serif(), sp, ImVec2(c.x - Width(Serif(), sp, sign) * 0.5f, c.y - sp * 0.56f), kInk, sign.c_str());
    float hp = f.L(38);
    const std::string head = Fit(Serif(), hp, v.popup_head, f.L(x1 - x0 - 120));
    Metal(dl, Serif(), hp, f.P(x0 + 90, y + 30), head, f.L(2), !v.popup_error);
    dl->AddText(Sans(), tp, f.P(x0 + 30, y + 92), kWhite, v.popup_text.c_str(), nullptr, room);
    const float lp = f.L(28);
    const float lw = 34 + 8 + Width(Sans(), lp, "Close") / f.s;
    const float lx = (x0 + x1) * 0.5f - lw * 0.5f, ly = y + h - 34;
    ButtonIcon(dl, f, f.P(lx + 17, ly), "A");
    Outlined(dl, Sans(), lp, f.P(lx + 42, ly - 19), kWhite, "Close", f.L(1.5f));
  }
};

MenuDialog* g_dialog = nullptr;

// the fonts and the overlay come before the link knows whether the menus are on: the environment says
bool Wanted() {
  const char* net = std::getenv("SC5_NET");
  const char* menus = std::getenv("SC5_MENUS");
  return net && menus && std::string(net) == "1" && std::string(menus) == "1";
}

ImFont* AddFont(ImFontAtlas* atlas, std::initializer_list<const char*> names) {
  char dir[MAX_PATH];
  const UINT n = GetWindowsDirectoryA(dir, MAX_PATH);
  if (!n || n > MAX_PATH - 32) return nullptr;
  for (const char* name : names) {
    const std::string file = std::string(dir) + "\\Fonts\\" + name;
    if (GetFileAttributesA(file.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
    ImFontConfig config;
    config.OversampleH = config.OversampleV = 1;
    static const ImWchar kRanges[] = {0x0020, 0x00FF, 0x2010, 0x2027, 0};
    if (ImFont* font = atlas->AddFontFromFileTTF(file.c_str(), 48.0f, &config, kRanges)) {
      REXLOG_INFO("sc5 online: font {}", name);
      return font;
    }
  }
  return nullptr;
}

}  // namespace

void AddFonts(ImFontAtlas* atlas) {
  if (!atlas || !Wanted()) return;
  g_serif = AddFont(atlas, {"palab.ttf", "georgiab.ttf", "timesbd.ttf"});
  g_sans = AddFont(atlas, {"seguisb.ttf", "segoeuib.ttf", "arialbd.ttf"});
}

void CreateOverlay(rex::ui::ImGuiDrawer* drawer) {
  if (!drawer || !Wanted() || g_dialog) return;
  g_dialog = new MenuDialog(drawer);  // a dialog keeps itself (rex/ui/imgui_dialog.h)
}

}  // namespace sc5::online
