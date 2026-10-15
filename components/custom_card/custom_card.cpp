#include "custom_card.h"
#include "icons.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace esphome::custom_card {

using tessera::Font;
using tessera::Tone;
namespace ui = tessera::ui;

// ---- What a template sensor describes ----
//
// The sensor's attribute `elements` is a list of texts, one part each, its fields split by "|":
//
//   value|<value>|<unit>|<label>|<colour>|<icon>
//   bar|<value>|<unit>|<label>|<colour>|<icon>|<min>|<max>
//   text|<words>|||<colour>|<icon>
//
// Fields at the end may be left out. A colour is one of the key colours of theme.h (red, orange, yellow, green, mint,
// blue, purple, pink, gray): it paints the part's icon (a dot where it has none) and its bar, never its words. The
// colours that mean something are Tessera's tones (plugin API 0.9), readable on the card in both looks: green is GOOD,
// yellow and orange BUSY, red ALERT, blue ACCENT; the others keep their key colour. An icon is a name of Tessera's set
// (icons.h).

// The tone a colour name means, as Tessera colours a level: good, moderate, bad, or the accent.
static Tone tone_of(const std::string &name) {
  if (name == "green") return Tone::GOOD;
  if (name == "yellow" || name == "orange") return Tone::BUSY;
  if (name == "red") return Tone::ALERT;
  if (name == "blue") return Tone::ACCENT;
  return Tone::NORMAL;
}

// WCAG's contrast of two colours, 1 to 21.
static float contrast(uint32_t a, uint32_t b) {
  auto lum = [](uint32_t c) {
    float sum = 0;
    const float weight[3] = {0.2126f, 0.7152f, 0.0722f};
    for (int i = 0; i < 3; ++i) {
      const float v = ((c >> (16 - 8 * i)) & 0xFF) / 255.0f;
      sum += weight[i] * (v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f));
    }
    return sum;
  };
  const float x = lum(a), y = lum(b);
  return (std::max(x, y) + 0.05f) / (std::min(x, y) + 0.05f);
}

// A part's colour as its icon: Tessera's own tile icon colour, and darker still where it would not read on the card
// (a yellow on white): Tessera's icons sit on a pale disc of their colour, these stand on the card itself.
static uint32_t readable(uint32_t colour) {
  const uint32_t card = theme::hex(theme::CARD);
  for (int i = 0; i < 8 && contrast(colour, card) < 3.0f; ++i) colour = theme::mix(colour, theme::hex(theme::INK), 205);
  return colour;
}
static uint32_t icon_colour(uint32_t colour) { return readable(theme::icon(colour)); }

static constexpr int MAX_PARTS = 8;
static constexpr int MAX_IN_ROW = 4;

enum class Kind : uint8_t { VALUE, BAR, TEXT };

struct Part {
  Kind kind = Kind::VALUE;
  std::string value, unit, label;
  uint32_t colour = 0;  // a key colour of theme.h, 0 for the card's own paint
  Tone tone = Tone::NORMAL;  // the tone the colour means, NORMAL for a colour without one
  uint32_t icon = 0;    // a codepoint of Tessera's set, 0 for none
  float min = 0, max = 100;
};

static std::string trim(const std::string &text) {
  const size_t start = text.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) return "";
  return text.substr(start, text.find_last_not_of(" \t\r\n") - start + 1);
}

static std::vector<std::string> fields_of(const std::string &line) {
  std::vector<std::string> out;
  size_t start = 0;
  while (true) {
    const size_t bar = line.find('|', start);
    out.push_back(trim(line.substr(start, bar == std::string::npos ? std::string::npos : bar - start)));
    if (bar == std::string::npos) return out;
    start = bar + 1;
  }
}

// An icon of Tessera's set by its name, with or without "mdi:"; 0 for one the screen's font does not hold.
static uint32_t icon_of(std::string name) {
  if (name.rfind("mdi:", 0) == 0) name = name.substr(4);
  if (name.empty()) return 0;
  const IconName *first = std::begin(ICON_NAMES), *last = std::end(ICON_NAMES);
  const IconName *at = std::lower_bound(first, last, name.c_str(),
                                        [](const IconName &icon, const char *key) { return strcmp(icon.name, key) < 0; });
  return at != last && name == at->name ? at->codepoint : 0;
}

// "21.4", "21,4" or " 820 " as a number; false for words (unknown, unavailable).
static bool number_of(std::string text, float *out) {
  std::replace(text.begin(), text.end(), ',', '.');
  text = trim(text);
  if (text.empty()) return false;
  char *end = nullptr;
  const float value = strtof(text.c_str(), &end);
  if (end == text.c_str() || *end != '\0' || !std::isfinite(value)) return false;
  *out = value;
  return true;
}

static bool parse(const std::string &line, Part *part) {
  const std::vector<std::string> f = fields_of(line);
  auto at = [&](size_t i) { return i < f.size() ? f[i] : std::string(); };
  if (f[0] == "value") part->kind = Kind::VALUE;
  else if (f[0] == "bar") part->kind = Kind::BAR;
  else if (f[0] == "text") part->kind = Kind::TEXT;
  else return false;
  part->value = at(1);
  part->unit = at(2);
  part->label = at(3);
  part->colour = theme::key_swatch(at(4));
  part->tone = tone_of(at(4));
  part->icon = icon_of(at(5));
  float n;
  if (number_of(at(6), &n)) part->min = n;
  if (number_of(at(7), &n)) part->max = n;
  return true;
}

// ---- The tile ----
//
// As Tessera's own plugins draw: words and numbers in the look's ink, a part's colour only as a mark (its icon, a dot
// where it has none, and its bar), the value's face chosen for the shape of the value so it never jumps a size when a
// digit changes, and the arrangement chosen for the shape of the tile: a tile one row high and two or more columns
// wide puts every part side by side, any other stacks them.

// The parts of a card on one drawing area: the tile's, or the card's a tap opens over the page. Both get the same data,
// the sensor's state, name and attributes.
class Drawing {
 public:
  explicit Drawing(const tessera::Plugin *plugin) : plugin_(plugin) {}

  // `titled`: the tile's name heads the parts (a card has its title in Tessera's frame instead).
  void create(lv_obj_t *parent, int width, int height, bool flat, const std::string &name, bool titled) {
    width_ = width;
    height_ = height;
    flat_ = flat;
    name_ = name;
    titled_ = titled;
    head_ = ui::label(parent, Font::TITLE, theme::INK);
    note_ = ui::label(parent, Font::BODY, theme::MUTED);
    lv_obj_set_style_text_align(note_, LV_TEXT_ALIGN_CENTER, 0);
    for (Slot &s : slots_) {
      s.icon = ui::label(parent, Font::ICON_SMALL, theme::MUTED);
      s.dot = ui::block(parent, theme::MUTED);
      lv_obj_set_style_radius(s.dot, LV_RADIUS_CIRCLE, 0);
      s.label = ui::label(parent, Font::BODY, theme::MUTED);
      s.value = ui::label(parent, Font::VALUE, theme::INK);
      s.unit = ui::label(parent, Font::BODY, theme::MUTED);
      s.track = ui::block(parent, theme::TRACK);
      s.fill = ui::block(s.track, theme::ACCENT);
    }
    hide_all();
  }

  // What the sensor says now: its parts, laid out and painted.
  void take(JsonObjectConst data) {
    parts_.clear();
    note_text_.clear();
    shown_name_ = !titled_ ? "" : !name_.empty() ? name_ : std::string(data["name"] | "");
    const std::string wait = data["wait"] | "";
    JsonVariantConst elements = data["attributes"]["elements"];
    bool described = false;
    if (elements.is<JsonArrayConst>()) {
      described = true;
      for (JsonVariantConst line : elements.as<JsonArrayConst>()) {
        Part part;
        if (parts_.size() < MAX_PARTS && line.is<const char *>() && parse(line.as<const char *>(), &part))
          parts_.push_back(part);
      }
    } else if (elements.is<const char *>()) {
      described = true;
      Part part;
      if (parse(elements.as<const char *>(), &part)) parts_.push_back(part);
    }
    const std::string state = data["state"] | "";
    if (!wait.empty()) {
      parts_.clear();
      note_text_ = plugin_->text(wait == "wrong_entity" ? "wrong_entity" : "waiting");
    } else if (described && parts_.empty()) {
      note_text_ = plugin_->text("no_elements");
    } else if (!described && state.empty()) {
      note_text_ = plugin_->text("choose");
    } else if (!described) {
      // A sensor without `elements`: its own state and unit, as a plain value.
      Part part;
      part.value = state;
      part.unit = data["attributes"]["unit_of_measurement"] | "";
      parts_.push_back(part);
    }
    layout();
    paint();
  }

  // The colours: words and numbers follow the look; a part's own colour paints only its mark and its bar.
  void paint() {
    ui::set_color(head_, theme::INK);
    ui::set_color(note_, theme::MUTED);
    for (size_t i = 0; i < MAX_PARTS; ++i) {
      Slot &s = slots_[i];
      const uint32_t own = i < parts_.size() ? parts_[i].colour : 0;
      const Tone tone = i < parts_.size() ? parts_[i].tone : Tone::NORMAL;
      ui::set_color(s.label, theme::MUTED);
      ui::set_color(s.unit, theme::MUTED);
      ui::set_color(s.value, theme::INK);
      lv_obj_set_style_bg_color(s.track, ui::color(theme::TRACK), 0);
      if (tone != Tone::NORMAL) {
        // The tone's colour; on the icon darker still where it would not read on the card (BUSY's amber on white).
        const uint32_t toned = lv_color_to_u32(ui::tone_color(tone, theme::MUTED));  // 0xFFRRGGBB: the alpha byte is ignored
        lv_obj_set_style_text_color(s.icon, theme::rgb(readable(toned)), 0);
        ui::set_fill(s.dot, tone, theme::ACCENT);
        ui::set_fill(s.fill, tone, theme::ACCENT);
      } else if (own) {
        lv_obj_set_style_text_color(s.icon, theme::rgb(icon_colour(own)), 0);
        lv_obj_set_style_bg_color(s.dot, theme::rgb(own), 0);
        lv_obj_set_style_bg_color(s.fill, theme::rgb(own), 0);
      } else {
        ui::set_color(s.icon, theme::MUTED);
        lv_obj_set_style_bg_color(s.fill, ui::color(theme::ACCENT), 0);
      }
    }
  }

 private:
  struct Slot {
    lv_obj_t *icon{}, *dot{}, *label{}, *value{}, *unit{}, *track{}, *fill{};
  };
  // Parts side by side in columns, or a bar or a text alone across the width (`across`: its words left, its value
  // right, the bar under them).
  struct Line {
    int first, count;
    bool across;
  };

  static void show(lv_obj_t *obj, bool shown) {
    if (shown) lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
  }

  void hide_all() {
    show(head_, false);
    show(note_, false);
    for (Slot &s : slots_)
      for (lv_obj_t *obj : {s.icon, s.dot, s.label, s.value, s.unit, s.track}) show(obj, false);
  }

  // Stacked: values that follow each other side by side (at most `in_row`), a bar and a text alone across the width.
  std::vector<Line> stacked(int in_row = MAX_IN_ROW) const {
    std::vector<Line> out;
    for (int i = 0; i < (int) parts_.size(); ++i) {
      const bool value = parts_[i].kind == Kind::VALUE;
      if (value && !out.empty() && !out.back().across && out.back().count < in_row)
        ++out.back().count;
      else
        out.push_back({i, 1, !value});
    }
    return out;
  }
  // Side by side: every part a column of its own, on a wide tile one row high.
  std::vector<Line> side_by_side() const {
    if (!flat_ || parts_.empty() || parts_.size() > MAX_IN_ROW) return {};
    return {{0, (int) parts_.size(), false}};
  }

  // The value's digits as zeros: the face is chosen for the shape of a value, never for its digits (as the departures
  // plugin does), so a value that ticks from 999 to 965 keeps its size.
  static std::string shape(std::string text) {
    for (char &c : text)
      if (c >= '1' && c <= '9') c = '0';
    return text;
  }

  // Larger faces first: DIGITS, then Font's own order.
  static int rank(Font f) { return f == Font::DIGITS ? -1 : static_cast<int>(f); }
  // Whether DIGITS has every character of every value shown large (a word, a letter or a sign it lacks keeps VALUE).
  bool digits_hold_values() const {
    for (const Part &part : parts_)
      if (part.kind != Kind::TEXT && !ui::covers(part.value, Font::DIGITS)) return false;
    return true;
  }
  int words_row() const { return std::max(ui::line_height(Font::BODY), ui::line_height(Font::ICON_SMALL)); }
  // The words of a text and of a bar across the width: one step above the body, and the body itself where the values
  // had to go down to it, so a tall list of texts shrinks with them instead of running out of the tile.
  static Font words_face(Font face) { return face == Font::BODY ? Font::BODY : Font::BODY_LARGE; }
  // A text's row: its words, and the small icon's height only where it has a mark.
  int text_row(const Part &part, Font face) const {
    const int words = ui::line_height(words_face(face));
    return has_mark(part) ? std::max(words, ui::line_height(Font::ICON_SMALL)) : words;
  }
  int bar_thickness() const { return ui::px(ui::large() ? 10 : 6); }
  int dot_size() const { return ui::px(ui::large() ? 12 : 8); }
  int gap() const { return ui::px(ui::large() ? 8 : 4); }
  int column_gap() const { return ui::px(ui::large() ? 16 : 8); }
  int column_width(int count) const { return (width_ - (count - 1) * column_gap()) / count; }

  // A part's mark: its icon, or a dot in its colour where it has no icon, or nothing.
  bool has_mark(const Part &part) const { return part.icon || part.colour; }
  bool has_words(const Line &line) const {
    for (int i = line.first; i < line.first + line.count; ++i)
      if (!parts_[i].label.empty() || has_mark(parts_[i])) return true;
    return false;
  }
  // A mark in front of a value, when there is no row for the words above it: the tile's icon size where the value is
  // that tall, else the small one.
  static Font inline_icon(Font face) {
    return ui::line_height(Font::ICON) <= ui::line_height(face) ? Font::ICON : Font::ICON_SMALL;
  }
  int mark_width(const Part &part, Font icon_face) const {
    return part.icon ? ui::text_width(ui::icon(part.icon), icon_face) : part.colour ? dot_size() : 0;
  }
  int unit_width(const Part &part) const {
    return part.unit.empty() ? 0 : ui::px(3) + ui::text_width(part.unit, Font::BODY);
  }

  int line_height(const Line &line, Font face, bool labels) const {
    const Part &first = parts_[line.first];
    if (line.across)
      return first.kind == Kind::BAR ? ui::line_height(words_face(face)) + ui::px(4) + bar_thickness()
                                     : text_row(first, face);
    const int words = labels && has_words(line) ? words_row() : 0;
    int h = 0;
    for (int i = line.first; i < line.first + line.count; ++i) {
      switch (parts_[i].kind) {
        case Kind::VALUE: h = std::max(h, words + ui::line_height(face)); break;
        case Kind::BAR: h = std::max(h, words + ui::line_height(face) + ui::px(4) + bar_thickness()); break;
        default: h = std::max(h, text_row(parts_[i], face));
      }
    }
    return h;
  }

  // Whether every value of `all` has the room of its column in `face`, whatever the height.
  bool fits_across(const std::vector<Line> &all, Font face, bool labels) const {
    for (const Line &line : all) {
      if (line.across) continue;
      const int column = column_width(line.count);
      for (int i = line.first; i < line.first + line.count; ++i) {
        const Part &part = parts_[i];
        if (part.kind == Kind::TEXT) {
          // A text beside other parts (a flat tile) has its words whole where another arrangement allows it.
          if (whole_texts_ && line.count > 1 && mark_width(part, Font::ICON_SMALL) + (has_mark(part) ? ui::px(6) : 0) +
                                        ui::text_width(part.value, words_face(face)) > column)
            return false;
          continue;
        }
        const int mark = labels || !has_mark(part) ? 0 : mark_width(part, inline_icon(face)) + ui::px(6);
        if (mark + ui::text_width(shape(part.value), face) + unit_width(part) > column) return false;
      }
    }
    return true;
  }

  bool fits(const std::vector<Line> &all, Font face, bool head, bool labels) const {
    if (!fits_across(all, face, labels)) return false;
    int used = head ? ui::line_height(Font::TITLE) + gap() : 0;
    for (const Line &line : all) used += line_height(line, face, labels);
    used += gap() * ((int) all.size() - 1);
    return used <= height_;
  }

  // Lay the parts out as the departures plugin does: the value takes the largest face that fits beside the tile's name,
  // and the row of labels stays only where it costs the value nothing (else a part's mark stands before its number).
  // The name goes only where no face fits with it. A wide flat tile puts the parts side by side when the value is no
  // smaller there than stacked.
  void layout() {
    hide_all();
    if (!note_text_.empty()) {
      const int h = ui::line_height(Font::BODY);
      lv_obj_set_size(note_, width_, h);
      lv_obj_set_pos(note_, 0, (height_ - h) / 2);
      ui::set_text(note_, note_text_);
      show(note_, true);
      return;
    }
    const std::vector<Line> arrangements[] = {side_by_side(), stacked()};
    const bool named = !shown_name_.empty();
    struct Try { bool head, labels; };
    Font face = Font::BODY;
    Try chosen = {false, false};
    const std::vector<Line> *lines = &arrangements[1];
    bool found = false;
    // A part whose label has no icon or colour to stand in for it keeps the row of labels, whatever it costs: without
    // it nothing would say what its number is.
    bool unmarked = false;
    for (const Part &part : parts_)
      if (part.kind != Kind::TEXT && !part.label.empty() && !has_mark(part)) unmarked = true;
    // Labels before whole words: first with a text beside other parts whole, then with it cut short, as before.
    for (bool labelled : {unmarked, false}) {
      for (bool whole : {true, false}) {
        whole_texts_ = whole;
        for (const std::vector<Line> &all : arrangements) {
          if (all.empty()) continue;
          for (bool head : {named, false}) {
            // The largest face for the values with the name alone (larger is earlier in Font, DIGITS before them
            // all, and only where every value is digits it holds).
            const Font every[] = {Font::DIGITS, Font::VALUE, Font::HEADLINE, Font::TITLE, Font::BODY_LARGE, Font::BODY};
            for (Font f : every) {
              if (f == Font::DIGITS && !digits_hold_values()) continue;
              if (!fits(all, f, head, labelled)) continue;
              if (!found || rank(f) < rank(face)) {
                face = f;
                chosen = {head, labelled || fits(all, f, head, true)};
                lines = &all;
                found = true;
              }
              break;
            }
            if (found) break;
          }
        }
        if (found) break;
      }
      if (found) break;
    }
    whole_texts_ = true;
    // Where nothing fits, the values two to a row, else one, so that no number is cut short; the lines that fit whole
    // show (below).
    if (!found)
      for (int in_row : {2, 1}) {
        fallback_ = stacked(in_row);
        lines = &fallback_;
        if (fits_across(fallback_, face, false)) break;
      }

    int y = 0;
    if (chosen.head) {
      const int h = ui::line_height(Font::TITLE);
      lv_obj_set_size(head_, width_, h);
      lv_obj_set_pos(head_, 0, 0);
      ui::set_text(head_, shown_name_);
      show(head_, true);
      y = h + gap();
    }
    int content = 0;
    for (const Line &line : *lines) content += line_height(line, face, chosen.labels);
    const int spare = std::max(0, height_ - y - content);
    const int n = (int) lines->size();
    const int between = n > 1 ? std::max(gap(), spare / (n + 1)) : 0;
    y += std::max(0, (spare - between * (n - 1)) / 2);
    // Where even the smallest face does not fit, the lines that fit whole, never one cut in half at the bottom.
    for (const Line &line : *lines) {
      const int h = line_height(line, face, chosen.labels);
      if (!found && y + h > height_) break;
      place(line, face, chosen.labels, y);
      y += h + between;
    }
  }

  // A part's mark at (x, y), centred on a row `h` high, in `icon_face`; returns its width.
  int place_mark(Slot &s, const Part &part, int x, int y, int h, Font icon_face) {
    if (part.icon) {
      const std::string glyph = ui::icon(part.icon);
      const int w = ui::text_width(glyph, icon_face), ih = ui::line_height(icon_face);
      ui::set_font(s.icon, icon_face);
      ui::set_text(s.icon, glyph);
      lv_obj_set_size(s.icon, w, ih);
      lv_obj_set_pos(s.icon, x, y + (h - ih) / 2);
      show(s.icon, true);
      return w;
    }
    if (part.colour) {
      const int d = dot_size();
      lv_obj_set_size(s.dot, d, d);
      lv_obj_set_pos(s.dot, x, y + (h - d) / 2);
      show(s.dot, true);
      return d;
    }
    return 0;
  }

  // The mark and words beside it, centred on a row `h` high at (x, y), at most `room` wide.
  void place_words(Slot &s, const Part &part, int x, int y, int h, int room, Font words_face, lv_obj_t *words,
                   const std::string &text) {
    const int mark = place_mark(s, part, x, y, h, Font::ICON_SMALL);
    const int left = x + (mark ? mark + ui::px(6) : 0);
    if (!text.empty() && x + room - left > 0) {
      const int wh = ui::line_height(words_face);
      ui::set_font(words, words_face);
      ui::set_text(words, text);
      lv_obj_set_size(words, x + room - left, wh);
      lv_obj_set_pos(words, left, y + (h - wh) / 2);
      show(words, true);
    }
  }

  // A value and its unit after it, on the value's baseline.
  void place_value(Slot &s, const Part &part, Font face, int x, int y, int room) {
    const int uw = unit_width(part);
    const int vw = std::min(ui::text_width(part.value, face), std::max(1, room - uw));
    const int vh = ui::line_height(face);
    ui::set_font(s.value, face);
    ui::set_text(s.value, part.value);
    lv_obj_set_size(s.value, vw, vh);
    lv_obj_set_pos(s.value, x, y);
    show(s.value, true);
    if (!part.unit.empty()) {
      const lv_font_t *big = ui::font(face), *small = ui::font(Font::BODY);
      const int uh = ui::line_height(Font::BODY);
      const int baseline = y + vh - (big ? big->base_line : 0);
      ui::set_font(s.unit, Font::BODY);
      ui::set_text(s.unit, part.unit);
      lv_obj_set_size(s.unit, uw - ui::px(3), uh);
      lv_obj_set_pos(s.unit, x + vw + ui::px(3), baseline - uh + (small ? small->base_line : 0));
      show(s.unit, true);
    }
  }

  // A bar `width` wide at (x, y), filled from min to max.
  void place_bar(Slot &s, const Part &part, int x, int y, int width) {
    const int thick = bar_thickness();
    lv_obj_set_size(s.track, width, thick);
    lv_obj_set_pos(s.track, x, y);
    lv_obj_set_style_radius(s.track, thick / 2, 0);
    lv_obj_set_style_radius(s.fill, thick / 2, 0);
    show(s.track, true);
    float value, share = 0;
    if (number_of(part.value, &value) && part.max > part.min)
      share = std::min(1.0f, std::max(0.0f, (value - part.min) / (part.max - part.min)));
    const int filled = share > 0 ? std::max(thick, (int) std::lround(share * width)) : 0;
    lv_obj_set_size(s.fill, filled, thick);
    lv_obj_set_pos(s.fill, 0, 0);
    show(s.fill, filled > 0);
  }

  void place(const Line &line, Font face, bool labels, int y) {
    const int h = line_height(line, face, labels);
    if (line.across) {
      const Part &part = parts_[line.first];
      Slot &s = slots_[line.first];
      if (part.kind == Kind::TEXT) {
        place_words(s, part, 0, y, h, width_, words_face(face), s.value, part.value);
        return;
      }
      // A bar across the width: its words at the left, the value at the right, the bar under them.
      const Font words = words_face(face);
      const int row = ui::line_height(words);
      const int right = ui::text_width(part.value, words) + unit_width(part);
      place_value(s, part, words, width_ - right, y, right);
      place_words(s, part, 0, y, row, std::max(0, width_ - right - ui::px(8)), Font::BODY, s.label, part.label);
      place_bar(s, part, 0, y + row + ui::px(4), width_);
      return;
    }
    const int column = column_width(line.count);
    const bool words = labels && has_words(line);
    for (int k = 0; k < line.count; ++k) {
      const Part &part = parts_[line.first + k];
      Slot &s = slots_[line.first + k];
      const int x = k * (column + column_gap());
      if (part.kind == Kind::TEXT) {
        const int th = text_row(part, face);
        place_words(s, part, x, y + (h - th) / 2, th, column, words_face(face), s.value, part.value);
        continue;
      }
      int vy = y;
      int room = 0;
      if (words) {
        place_words(s, part, x, y, words_row(), column, Font::BODY, s.label, part.label);
        vy += words_row();
      } else if (has_mark(part)) {
        // No row for the words: the mark stands before the number, centred on it, and says what it is.
        room = place_mark(s, part, x, y, ui::line_height(face), inline_icon(face)) + ui::px(6);
      }
      place_value(s, part, face, x + room, vy, column - room);
      if (part.kind == Kind::BAR) place_bar(s, part, x, vy + ui::line_height(face) + ui::px(4), column);
    }
  }

  const tessera::Plugin *plugin_;
  int width_ = 0, height_ = 0;
  bool flat_ = false, titled_ = true;
  std::string name_, shown_name_, note_text_;
  std::vector<Part> parts_;
  std::vector<Line> fallback_;
  mutable bool whole_texts_ = true;  // layout's choice while it searches (fits_across reads it)  // the arrangement of a tile where nothing fits, which `lines` may point to
  Slot slots_[MAX_PARTS];
  lv_obj_t *head_{}, *note_{};
};

// The tile: its parts on the card's area; a tap opens them large over the page.
class CardTile : public tessera::Tile {
 public:
  explicit CardTile(const tessera::Plugin *plugin) : plugin_(plugin), drawing_(plugin) {}

  void create(const tessera::TileContext &c) override {
    entity_ = c.entity ? c.entity : "";
    tile_ = c.tile;
    tap_script_ = c.options["tap_script"] | "";
    // A tile one row high and two columns or more puts its parts side by side, as the departures plugin turns into a
    // list on two columns.
    drawing_.create(c.parent, c.width, c.height, c.rows == 1 && c.columns >= 2, c.name ? c.name : "", true);
  }
  void on_state(JsonObjectConst data) override { drawing_.take(data); }
  void on_theme() override { drawing_.paint(); }
  void on_tap() override {
    if (!tap_script_.empty()) {
      tessera::action("script.turn_on", tap_script_);
    } else {
      tessera::open_card(plugin_->plugin_id(), "detail", entity_, tile_);
    }
  }

 private:
  const tessera::Plugin *plugin_;
  Drawing drawing_;
  std::string entity_;
  std::string tap_script_;
  int tile_ = -1;
};

// The card a tap on the tile opens: the same parts, as large as a card's room takes, with their labels where they fit.
// Tessera draws its frame, its back key and the tile's name as its title.
class DetailCard : public tessera::Card {
 public:
  explicit DetailCard(const tessera::Plugin *plugin) : drawing_(plugin) {}

  void open(const tessera::CardContext &c) override {
    drawing_.create(c.parent, c.width, c.height, false, "", false);
  }
  void on_state(JsonObjectConst data) override { drawing_.take(data); }
  void on_theme() override { drawing_.paint(); }

 private:
  Drawing drawing_;
};

void CustomCard::setup() {
  add_tile("card", [this]() { return new CardTile(this); });
  add_card("detail", [this]() { return new DetailCard(this); });
}

}  // namespace esphome::custom_card
