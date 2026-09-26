// Visual theme for the runtime's own prompts (friend picker, game invites,
// notices, message boxes, keyboard input): a dimmed screen with a centred
// dark panel under a slanted title plate, full-width selection bars and
// controller prompts in the corner, in the style of a sports title's
// frontend. Everything is sized from the screen height, so a prompt looks
// the same at 720p and 4K.
//
// Usage inside an ImGuiDialog::OnDraw:
//   theme::Panel panel("GAME INVITE", "XBOX LIVE");
//   panel.Text("Pedro2 invited you to play.");
//   if (panel.Bar("ACCEPT", focus == 0)) ...
//   panel.Prompts({{'A', "SELECT"}, {'B', "BACK"}});
//   (the panel finishes drawing when it goes out of scope)
#pragma once

#include <initializer_list>
#include <utility>

#include <imgui.h>

namespace rex::kernel::xam::theme {

class Panel {
 public:
  // `title` goes on the plate (shown upper case); `kicker` is the small line
  // above it. `width` is in 1080p pixels. `appeared_at` (ImGui time of the
  // first draw) drives the fade-in; pass the value you stored.
  Panel(const char* title, const char* kicker, float width = 760.0f, double appeared_at = -1.0);
  ~Panel();

  // Wrapped body text.
  void Text(const char* text);
  // Secondary, dimmer text.
  void Hint(const char* text);
  // A full-width choice bar; `detail` is drawn right-aligned (optional),
  // `detail_color` tints it. Returns true when clicked. Hovering with the
  // mouse reports through hovered().
  bool Bar(const char* label, bool selected, const char* detail = nullptr,
           ImU32 detail_color = 0);
  // Vertical gap in 1080p pixels.
  void Space(float units);
  // Controller prompts along the bottom edge, e.g. {{'A', "INVITE"}, {'B', "BACK"}}.
  void Prompts(std::initializer_list<std::pair<char, const char*>> prompts);
  // Index of the bar the mouse is over this frame, or -1.
  int hovered() const { return hovered_; }
  // Screen-space width available for content (for custom widgets).
  float content_width() const { return inner_width_; }
  // Position the ImGui cursor at the next content slot (for custom widgets)
  // and report the height they used afterwards.
  void BeginCustom();
  void EndCustom();
  float scale() const { return s_; }

 private:
  float s_ = 1.0f;
  float alpha_ = 1.0f;
  float x_ = 0.0f, y_ = 0.0f;  // content cursor (screen space)
  float left_ = 0.0f, top_ = 0.0f, width_ = 0.0f, inner_width_ = 0.0f;
  int bar_index_ = 0;
  int hovered_ = -1;
  float custom_start_ = 0.0f;
  ImDrawList* bg_ = nullptr;   // panel background (drawn after layout)
  ImDrawListSplitter split_;
};

// Colours used by the theme, for callers drawing custom content.
ImU32 Accent(float alpha = 1.0f);
ImU32 TextColor(float alpha = 1.0f);
ImU32 DimTextColor(float alpha = 1.0f);
ImU32 OnlineColor(float alpha = 1.0f);

}  // namespace rex::kernel::xam::theme
