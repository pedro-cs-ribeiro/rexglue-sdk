// Visual theme for the runtime's own prompts; see xam_dialog_theme.h.

#include "xam_dialog_theme.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <string>
#include <vector>

#include <rex/ui/imgui_drawer.h>

namespace rex::kernel::xam::theme {

namespace {

// Palette (1080p design units below are multiplied by the screen scale).
constexpr ImU32 Rgba(int r, int g, int b, float a) {
  return IM_COL32(r, g, b, static_cast<int>(a * 255.0f + 0.5f));
}

ImFont* FontOr(const char* name) {
  ImFont* font = rex::ui::FindUIFont(name);
  return font ? font : ImGui::GetFont();
}

std::string Upper(const char* text) {
  std::string out(text ? text : "");
  for (char& c : out) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return out;
}

constexpr float kPad = 40.0f;         // inner padding
constexpr float kBarHeight = 78.0f;   // choice bar
constexpr float kBarGap = 10.0f;
constexpr float kBodySize = 36.0f;
constexpr float kHintSize = 28.0f;
constexpr float kBarFontSize = 46.0f;
constexpr float kTitleSize = 70.0f;
constexpr float kKickerSize = 26.0f;
constexpr float kPromptSize = 30.0f;

}  // namespace

ImU32 Accent(float alpha) { return Rgba(236, 186, 24, alpha); }
ImU32 TextColor(float alpha) { return Rgba(240, 240, 240, alpha); }
ImU32 DimTextColor(float alpha) { return Rgba(160, 160, 160, alpha); }
ImU32 OnlineColor(float alpha) { return Rgba(120, 200, 70, alpha); }

Panel::Panel(const char* title, const char* kicker, float width, double appeared_at) {
  ImGuiIO& io = ImGui::GetIO();
  s_ = std::max(0.5f, io.DisplaySize.y / 1080.0f);
  const double now = ImGui::GetTime();
  const float t = appeared_at < 0.0 ? 1.0f : static_cast<float>((now - appeared_at) / 0.18);
  alpha_ = std::clamp(t, 0.0f, 1.0f);

  ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
  ImGui::SetNextWindowSize(io.DisplaySize);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::Begin("##rex-prompt", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoNav);
  ImGui::PopStyleVar(2);
  bg_ = ImGui::GetWindowDrawList();
  split_.Split(bg_, 2);
  split_.SetCurrentChannel(bg_, 1);

  // Keep the panel centred: the height comes from the previous frame.
  ImGuiStorage* storage = ImGui::GetStateStorage();
  const float last_height = storage->GetFloat(ImGui::GetID("##height"), 420.0f * s_);
  width_ = std::min(width * s_, io.DisplaySize.x - 64.0f * s_);
  left_ = (io.DisplaySize.x - width_) * 0.5f;
  const float plate_height = 128.0f * s_;
  const float slide = (1.0f - alpha_) * 24.0f * s_;
  const float total = plate_height + last_height;
  top_ = std::max(16.0f * s_, (io.DisplaySize.y - total) * 0.5f) + plate_height + slide;
  inner_width_ = width_ - 2.0f * kPad * s_;

  // Title plate: a light slanted card above the panel's top-left corner.
  ImFont* heading = FontOr("rex-heading");
  ImFont* label = FontOr("rex-label");
  const std::string title_text = Upper(title);
  const std::string kicker_text = Upper(kicker);
  const ImVec2 title_size = heading->CalcTextSizeA(kTitleSize * s_, FLT_MAX, 0.0f, title_text.c_str());
  const ImVec2 kicker_size = label->CalcTextSizeA(kKickerSize * s_, FLT_MAX, 0.0f, kicker_text.c_str());
  const float plate_w = std::max(title_size.x, kicker_size.x) + 90.0f * s_;
  const float px = left_ - 18.0f * s_;
  const float py = top_ - plate_height + 6.0f * s_;
  const float skew = 34.0f * s_;
  ImDrawList* dl = bg_;
  const ImVec2 plate[4] = {ImVec2(px, py), ImVec2(px + plate_w + skew, py),
                           ImVec2(px + plate_w, py + plate_height - 14.0f * s_),
                           ImVec2(px, py + plate_height - 14.0f * s_)};
  dl->AddConvexPolyFilled(plate, 4, Rgba(222, 222, 218, 0.97f * alpha_));
  // A thin accent stripe under the plate, like the frontend's header tabs.
  dl->AddRectFilled(ImVec2(px, py + plate_height - 14.0f * s_),
                    ImVec2(px + plate_w * 0.55f, py + plate_height - 8.0f * s_), Accent(alpha_));
  const float text_x = px + 30.0f * s_;
  if (!kicker_text.empty()) {
    dl->AddText(label, kKickerSize * s_, ImVec2(text_x, py + 10.0f * s_), Rgba(90, 90, 90, alpha_),
                kicker_text.c_str());
  }
  dl->AddText(heading, kTitleSize * s_,
              ImVec2(text_x, py + (kicker_text.empty() ? 22.0f : 34.0f) * s_),
              Rgba(24, 24, 24, alpha_), title_text.c_str());

  x_ = left_ + kPad * s_;
  y_ = top_ + kPad * s_;
}

void Panel::Text(const char* text) {
  ImFont* body = FontOr("rex-body");
  const float size = kBodySize * s_;
  const ImVec2 dims = body->CalcTextSizeA(size, FLT_MAX, inner_width_, text);
  bg_->AddText(body, size, ImVec2(x_, y_), TextColor(alpha_), text, nullptr, inner_width_);
  y_ += dims.y + 18.0f * s_;
}

void Panel::Hint(const char* text) {
  ImFont* body = FontOr("rex-body");
  const float size = kHintSize * s_;
  const ImVec2 dims = body->CalcTextSizeA(size, FLT_MAX, inner_width_, text);
  bg_->AddText(body, size, ImVec2(x_, y_), DimTextColor(alpha_), text, nullptr, inner_width_);
  y_ += dims.y + 14.0f * s_;
}

bool Panel::Bar(const char* label_text, bool selected, const char* detail, ImU32 detail_color) {
  const float h = kBarHeight * s_;
  const ImVec2 a(x_, y_);
  const ImVec2 b(x_ + inner_width_, y_ + h);
  ImGui::SetCursorScreenPos(a);
  ImGui::PushID(bar_index_);
  const bool clicked = ImGui::InvisibleButton("##bar", ImVec2(inner_width_, h));
  if (ImGui::IsItemHovered()) {
    hovered_ = bar_index_;
  }
  ImGui::PopID();

  ImDrawList* dl = bg_;
  if (selected) {
    dl->AddRectFilled(a, b, Accent(alpha_));
    // Slanted dark notch on the left edge, echoing the frontend's menu bars.
    const ImVec2 notch[4] = {ImVec2(a.x, a.y), ImVec2(a.x + 14.0f * s_, a.y),
                             ImVec2(a.x + 4.0f * s_, b.y), ImVec2(a.x, b.y)};
    dl->AddConvexPolyFilled(notch, 4, Rgba(20, 20, 20, alpha_));
  } else {
    dl->AddRectFilled(a, b, Rgba(38, 38, 38, 0.96f * alpha_));
  }
  ImFont* heading = FontOr("rex-heading");
  const float size = kBarFontSize * s_;
  const std::string text = Upper(label_text);
  const ImVec2 text_size = heading->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str());
  dl->AddText(heading, size, ImVec2(a.x + 34.0f * s_, a.y + (h - text_size.y) * 0.5f),
              selected ? Rgba(16, 16, 16, alpha_) : Rgba(215, 215, 215, alpha_), text.c_str());
  if (detail && *detail) {
    ImFont* label = FontOr("rex-label");
    const float dsize = 26.0f * s_;
    const std::string detail_text = Upper(detail);
    const ImVec2 dsz = label->CalcTextSizeA(dsize, FLT_MAX, 0.0f, detail_text.c_str());
    ImU32 color = detail_color ? detail_color
                               : (selected ? Rgba(40, 40, 40, alpha_) : DimTextColor(alpha_));
    if (detail_color && selected) {
      color = Rgba(30, 60, 10, alpha_);
    }
    const float dx = b.x - 28.0f * s_ - dsz.x;
    const float dy = a.y + (h - dsz.y) * 0.5f;
    if (detail_color) {
      dl->AddCircleFilled(ImVec2(dx - 16.0f * s_, a.y + h * 0.5f), 6.0f * s_, color);
    }
    dl->AddText(label, dsize, ImVec2(dx, dy), color, detail_text.c_str());
  }
  y_ += h + kBarGap * s_;
  ++bar_index_;
  return clicked;
}

void Panel::Space(float units) { y_ += units * s_; }

void Panel::BeginCustom() {
  ImGui::SetCursorScreenPos(ImVec2(x_, y_));
  custom_start_ = y_;
}

void Panel::EndCustom() {
  y_ = std::max(y_, ImGui::GetCursorScreenPos().y) + 12.0f * s_;
}

void Panel::Prompts(std::initializer_list<std::pair<char, const char*>> prompts) {
  // Drawn under the panel, right-aligned, after the panel's height is known.
  ImFont* label = FontOr("rex-label");
  const float size = kPromptSize * s_;
  const float r = 19.0f * s_;
  const float bottom = y_ + (kPad - kBarGap) * s_;
  float x = left_ + width_;
  const float cy = bottom + 40.0f * s_;
  // Right to left so the last prompt ends at the panel's right edge.
  std::vector<std::pair<char, const char*>> items(prompts);
  for (auto it = items.rbegin(); it != items.rend(); ++it) {
    const std::string text = Upper(it->second);
    const ImVec2 tsz = label->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str());
    x -= tsz.x;
    bg_->AddText(label, size, ImVec2(x, cy - tsz.y * 0.5f), TextColor(alpha_), text.c_str());
    x -= 12.0f * s_ + r;
    bg_->AddCircleFilled(ImVec2(x, cy), r, Rgba(245, 245, 245, alpha_));
    const char glyph[2] = {it->first, 0};
    const ImVec2 gsz = label->CalcTextSizeA(size * 0.9f, FLT_MAX, 0.0f, glyph);
    bg_->AddText(label, size * 0.9f, ImVec2(x - gsz.x * 0.5f, cy - gsz.y * 0.5f),
                 Rgba(20, 20, 20, alpha_), glyph);
    x -= r + 36.0f * s_;
  }
}

Panel::~Panel() {
  // Panel background behind everything drawn so far.
  const float bottom = y_ + (kPad - kBarGap) * s_;
  split_.SetCurrentChannel(bg_, 0);
  ImGuiIO& io = ImGui::GetIO();
  bg_->AddRectFilled(ImVec2(0.0f, 0.0f), io.DisplaySize, IM_COL32(0, 0, 0, int(150 * alpha_)));
  bg_->AddRectFilled(ImVec2(left_, top_), ImVec2(left_ + width_, bottom),
                     Rgba(22, 22, 22, 0.97f * alpha_));
  bg_->AddRect(ImVec2(left_, top_), ImVec2(left_ + width_, bottom), Rgba(70, 70, 70, alpha_), 0.0f,
               0, 2.0f * s_);
  split_.Merge(bg_);
  ImGui::GetStateStorage()->SetFloat(ImGui::GetID("##height"), bottom - top_ + 80.0f * s_);
  ImGui::End();
}

}  // namespace rex::kernel::xam::theme
