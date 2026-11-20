#include "ui/controls/shape_labels.h"

#include "render/core/renderer.h"
#include "render/scene/text_node.h"
#include "ui/controls/glyph.h"
#include "workbench/nest_labels.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <numbers>
#include <string>
#include <utility>

namespace {
  using luciole::workbench::FileCategory;
  using luciole::workbench::LabelAlign;
  using luciole::workbench::LabelFit;
  using luciole::workbench::LabelTrail;
  using luciole::workbench::LabelWeight;
  using luciole::workbench::Rgba8;
  using luciole::workbench::ShapeLabel;
  using luciole::workbench::TrailPlace;

  constexpr float kRadian = std::numbers::pi_v<float> / 180.0F;
  // Room past a laid-out width that keeps rounding in the layout from cutting the text.
  constexpr float kLayoutSlack = 1;

  Color textColor(Rgba8 color) { return rgba(color.r / 255.0F, color.g / 255.0F, color.b / 255.0F, color.a / 255.0F); }

  float mix(float from, float to, float t) { return from + (to - from) * t; }

  // The text a label shows in `width`, or empty.
  std::string fitted(const ShapeLabel& label, float width, const LabelMeasure& measure) {
    const double size = label.fontSize;
    const auto weight = label.weight;
    const auto fits = [&](std::string_view text) { return !text.empty() && measure(text, size, weight) <= width; };
    if (label.fit == LabelFit::Shorten) {
      return luciole::workbench::fitNestText(label.text, width, [&](std::string_view text) {
        return measure(text, size, weight);
      });
    }
    if (fits(label.text)) {
      return label.text;
    }
    return fits(label.shortText) ? label.shortText : std::string{};
  }

  // A whole label for a trail, in a box as tall for its font as the label's box is for its own.
  ShapeLabel trailLabel(const ShapeLabel& label, const LabelTrail& trail, float x, float middle, float width) {
    const float height = label.fontSize > 0 ? label.height * trail.fontSize / label.fontSize : 0;
    return {
        .node = label.node,
        .text = trail.text,
        .x = x,
        .y = middle - height / 2,
        .width = width,
        .height = height,
        .align = trail.place == TrailPlace::End ? LabelAlign::End : label.align,
        .fontSize = trail.fontSize,
        .rotation = label.rotation,
        .color = trail.color,
        .fit = LabelFit::Whole,
    };
  }
} // namespace

void fitShapeLabels(std::vector<ShapeLabel>& labels, const LabelMeasure& measure) {
  std::vector<ShapeLabel> out;
  out.reserve(labels.size());
  for (auto& label : labels) {
    if (!std::isfinite(label.width) || label.width <= 0) {
      continue;
    }
    const auto trail = std::exchange(label.trail, std::nullopt);
    const float middle = label.y + label.height / 2;
    float trailWidth = 0;
    if (trail && trail->place == TrailPlace::End) {
      trailWidth = static_cast<float>(measure(trail->text, trail->fontSize, LabelWeight::Regular));
    }
    const bool beside = trail && trail->place == TrailPlace::End && trailWidth + trail->gap < trail->room;
    // The measure goes first: a text that cannot show beside it takes the whole line.
    auto text = beside ? fitted(label, label.width - trailWidth - trail->gap, measure) : std::string{};
    const bool besideShown = !text.empty();
    if (!besideShown) {
      text = fitted(label, label.width, measure);
    }
    if (text.empty()) {
      continue;
    }
    const float boxWidth = label.width;
    // A count down to its short form keeps no line under it.
    const bool shortForm = label.fit == LabelFit::Whole && text != label.text;
    label.text = std::move(text);
    if (besideShown) {
      label.width -= trailWidth + trail->gap;
    }
    out.push_back(std::move(label));
    const auto& shown = out.back();
    if (besideShown) {
      out.push_back(trailLabel(shown, *trail, shown.x, middle, boxWidth));
    } else if (
        trail
        && trail->place == TrailPlace::Below
        && !shortForm
        && measure(trail->text, trail->fontSize, LabelWeight::Regular) <= trail->width
    ) {
      out.push_back(trailLabel(shown, *trail, shown.x + trail->dx, middle + trail->dy, trail->width));
    }
  }
  labels = std::move(out);
}

std::string_view categoryGlyph(FileCategory category) {
  switch (category) {
  case FileCategory::Folder:
    return "folder";
  case FileCategory::Image:
    return "photo";
  case FileCategory::Video:
    return "movie";
  case FileCategory::Audio:
    return "music";
  case FileCategory::Document:
    return "file-text";
  case FileCategory::Code:
    return "code";
  case FileCategory::Archive:
    return "file-zip";
  case FileCategory::Data:
    return "table";
  case FileCategory::Executable:
    return "terminal-2";
  case FileCategory::Other:
    break;
  }
  return "file";
}

ShapeLabels::ShapeLabels(Node& layer) : m_layer(layer) { m_slots.reserve(kCap); }

ShapeLabels::Pose ShapeLabels::poseOf(const ShapeLabel& label, float opacity) {
  return {
      .x = label.x,
      .middle = label.y + label.height / 2,
      .width = label.width,
      .rotation = label.rotation,
      .color = textColor(label.color),
      .opacity = opacity,
      .iconX = label.x + label.icon.left,
      .iconY = label.y + label.icon.top,
  };
}

ShapeLabels::Pose ShapeLabels::mixed(const Pose& from, const Pose& to, float t) {
  return {
      .x = mix(from.x, to.x, t),
      .middle = mix(from.middle, to.middle, t),
      .width = mix(from.width, to.width, t),
      .rotation = mix(from.rotation, to.rotation, t),
      .color = lerpColor(from.color, to.color, t),
      .opacity = std::clamp(mix(from.opacity, to.opacity, t), 0.0F, 1.0F),
      .iconX = mix(from.iconX, to.iconX, t),
      .iconY = mix(from.iconY, to.iconY, t),
  };
}

ShapeLabels::Slot* ShapeLabels::freeSlot() {
  for (auto& slot : m_slots) {
    if (slot.motion == Motion::Hidden) {
      return &slot;
    }
  }
  if (m_slots.size() < kCap) {
    auto node = std::make_unique<TextNode>();
    node->setHitTestVisible(false);
    node->setMaxLines(1);
    auto* text = static_cast<TextNode*>(m_layer.addChild(std::move(node)));
    return &m_slots.emplace_back(Slot{.node = text});
  }
  for (auto& slot : m_slots) {
    if (slot.motion == Motion::Leaving) {
      return &slot;
    }
  }
  return nullptr;
}

void ShapeLabels::assign(Slot& slot, const ShapeLabel& label, Motion motion, const Pose& from) {
  slot.motion = motion;
  slot.label = label.node;
  slot.from = from;
  slot.to = poseOf(label, 1);
  slot.align = label.align;
  slot.iconSpec = label.icon;
  slot.measured = false;
  slot.node->setText(label.text);
  slot.node->setFontSize(label.fontSize);
  slot.node->setFontWeight(label.weight == LabelWeight::Semibold ? FontWeight::SemiBold : FontWeight::Normal);
  slot.node->setMaxWidth(std::max(label.width, 0.0F));
  slot.node->setVisible(true);
  showIcon(slot);
  apply(slot);
}

void ShapeLabels::showIcon(Slot& slot) {
  const auto& spec = slot.iconSpec;
  const bool wanted = spec.size > 0;
  if (wanted && slot.icon == nullptr) {
    auto icon = std::make_unique<Glyph>();
    icon->setHitTestVisible(false);
    slot.icon = static_cast<Glyph*>(m_layer.addChild(std::move(icon)));
  }
  if (slot.icon == nullptr) {
    return;
  }
  slot.icon->setVisible(wanted);
  if (wanted) {
    (void)slot.icon->setGlyph(categoryGlyph(spec.category));
    slot.icon->setGlyphSize(spec.size);
    slot.icon->setColor(textColor(spec.color));
  }
}

void ShapeLabels::hide(Slot& slot) {
  slot.motion = Motion::Hidden;
  slot.node->setVisible(false);
  if (slot.icon != nullptr) {
    slot.icon->setVisible(false);
  }
}

void ShapeLabels::measure(Renderer& renderer, Slot& slot) {
  auto& text = *slot.node;
  slot.capHeight = renderer.measureFont(text.fontSize(), text.font()).capHeight;
  // The text was fitted to its box with summed advances, which shaping can exceed by a pixel or two: the node's
  // maximum width then grows to the laid-out width, so the node never cuts a fitted text again.
  const auto metrics = renderer.measureText(text.text(), text.fontSize(), text.font(), 0, text.maxLines());
  text.setMaxWidth(std::max(slot.to.width, metrics.width + kLayoutSlack));
  slot.textWidth = metrics.width;
  // Text without ink, such as spaces, is centered by its advance.
  slot.inkMiddle = metrics.inkRight > metrics.inkLeft ? (metrics.inkLeft + metrics.inkRight) / 2 : metrics.width / 2;
  if (slot.icon != nullptr && slot.icon->visible()) {
    const float size = slot.iconSpec.size;
    slot.icon->arrange(renderer, {0, 0, size, size});
  }
  slot.measured = true;
}

void ShapeLabels::apply(Slot& slot) const {
  if (slot.motion == Motion::Hidden) {
    return;
  }
  const Pose pose = mixed(slot.from, slot.to, m_progress);
  float inset = 0;
  switch (slot.align) {
  case LabelAlign::Start:
    break;
  case LabelAlign::Center:
    inset = pose.width / 2 - slot.inkMiddle;
    break;
  case LabelAlign::End:
    inset = pose.width - slot.textWidth;
    break;
  }
  // A text node draws from its baseline; the cap band is centered on the box middle.
  slot.node->setPosition(pose.x + inset, pose.middle + slot.capHeight / 2);
  slot.node->setTransformOrigin(pose.width / 2 - inset, -slot.capHeight / 2);
  slot.node->setRotation(pose.rotation * kRadian);
  slot.node->setColor(pose.color);
  slot.node->setOpacity(pose.opacity);
  if (slot.icon != nullptr && slot.icon->visible()) {
    slot.icon->setPosition(pose.iconX, pose.iconY);
    slot.icon->setOpacity(pose.opacity);
  }
}

void ShapeLabels::show(std::span<const ShapeLabel> labels) {
  m_progress = 1;
  for (auto& slot : m_slots) {
    slot.motion = Motion::Hidden;
  }
  for (const auto& label : labels.first(std::min(labels.size(), kCap))) {
    if (auto* slot = freeSlot()) {
      assign(*slot, label, Motion::Shown, poseOf(label, 1));
    }
  }
  for (auto& slot : m_slots) {
    if (slot.motion == Motion::Hidden) {
      hide(slot);
    }
  }
}

void ShapeLabels::beginTransition(std::span<const ShapeLabel> labels) {
  // Every drawn label starts from where it is now and fades out unless a new label keeps it.
  for (auto& slot : m_slots) {
    if (slot.motion == Motion::Hidden) {
      continue;
    }
    slot.from = mixed(slot.from, slot.to, m_progress);
    slot.to = slot.from;
    slot.to.opacity = 0;
    slot.motion = Motion::Leaving;
  }
  m_progress = 0;
  std::vector<const ShapeLabel*> entering;
  for (const auto& label : labels.first(std::min(labels.size(), kCap))) {
    const auto kept = std::ranges::find_if(m_slots, [&label](const Slot& slot) {
      return slot.motion == Motion::Leaving && slot.label == label.node && slot.node->text() == label.text;
    });
    if (kept == m_slots.end()) {
      entering.push_back(&label);
    } else {
      assign(*kept, label, Motion::Moving, kept->from);
    }
  }
  for (const auto* label : entering) {
    if (auto* slot = freeSlot()) {
      assign(*slot, *label, Motion::Moving, poseOf(*label, 0));
    }
  }
  for (auto& slot : m_slots) {
    apply(slot);
  }
}

void ShapeLabels::setProgress(float progress) {
  m_progress = std::clamp(progress, 0.0F, 1.0F);
  for (auto& slot : m_slots) {
    apply(slot);
  }
}

void ShapeLabels::finish() {
  m_progress = 1;
  for (auto& slot : m_slots) {
    if (slot.motion == Motion::Leaving) {
      hide(slot);
    } else if (slot.motion != Motion::Hidden) {
      slot.motion = Motion::Shown;
      slot.from = slot.to;
      apply(slot);
    }
  }
}

void ShapeLabels::place(Renderer& renderer) {
  const float scale = renderer.renderScale();
  const bool rescaled = scale != m_measuredScale;
  m_measuredScale = scale;
  for (auto& slot : m_slots) {
    if (slot.motion == Motion::Hidden) {
      continue;
    }
    if (rescaled || !slot.measured) {
      measure(renderer, slot);
    }
    apply(slot);
  }
}
