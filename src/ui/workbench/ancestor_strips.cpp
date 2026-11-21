#include "ui/workbench/ancestor_strips.h"

#include "core/input/key_symbols.h"
#include "render/scene/input_area.h"
#include "ui/controls/box.h"
#include "ui/controls/glyph.h"
#include "ui/controls/label.h"
#include "ui/controls/pane_divider.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <array>
#include <format>
#include <initializer_list>
#include <memory>
#include <numbers>
#include <string>
#include <string_view>
#include <utility>

namespace luciole {
  namespace {
    namespace wb = workbench;

    // What a band carries: an icon (0 for none) and a name at a font size.
    struct Face {
      float icon = 0;
      float font = 0;
    };
    constexpr std::array<Face, 4> kFaces{
        {{.icon = 18, .font = 13}, {.icon = 16, .font = 13}, {.icon = 0, .font = 13}, {.icon = 0, .font = 11}}
    };
    constexpr float kStackFont = 11;
    constexpr float kTopPadding = 14;
    constexpr float kBottomPadding = 24;
    constexpr float kFaceGap = 10;
    constexpr float kQuarterTurn = std::numbers::pi_v<float> / 2; // names read from top to bottom
    constexpr std::string_view kEdgeTip =
        "Resize the ancestor strips; Left/Right adjusts, Home or a double-click resets";

    // The face of a band by its level.
    std::optional<Face> faceOf(const wb::Strip& strip) {
      if (strip.kind == wb::StripKind::Stack) {
        return Face{.icon = 0, .font = kStackFont};
      }
      if (strip.level == 0) {
        return std::nullopt;
      }
      return kFaces[std::min(strip.level - 1, kFaces.size() - 1)];
    }

    // Strips grow quieter with distance.
    ColorRole quietFill(const wb::Strip& strip) {
      if (strip.kind == wb::StripKind::Stack) {
        return ColorRole::SurfaceContainerLow;
      }
      switch (strip.level) {
      case 1:
        return ColorRole::SurfaceContainerHighest;
      case 2:
        return ColorRole::SurfaceContainerHigh;
      case 3:
        return ColorRole::SurfaceContainer;
      default:
        return ColorRole::SurfaceContainerLow;
      }
    }

    ColorRole quietText(const wb::Strip& strip) {
      return strip.kind == wb::StripKind::Ancestor && strip.level <= 2 ? ColorRole::OnSurface
                                                                       : ColorRole::OnSurfaceVariant;
    }

    template <class T> T* adopt(Node& parent, std::unique_ptr<T> child) {
      return static_cast<T*>(parent.addChild(std::move(child)));
    }
  } // namespace

  AncestorStrips::AncestorStrips() { m_slots.reserve(kMaxStrips); }

  void AncestorStrips::setAncestors(std::vector<AncestorEntry> ancestors) {
    m_ancestors = std::move(ancestors);
    bind();
  }

  void AncestorStrips::setPlan(std::vector<wb::Strip> plan) {
    m_plan = std::move(plan);
    bind();
  }

  void AncestorStrips::setTextScale(float scale) {
    if (scale != m_textScale && scale > 0) {
      m_textScale = scale;
      bind();
    }
  }

  void AncestorStrips::setPointed(std::optional<std::uint64_t> id) {
    m_pointed = id;
    for (std::size_t i = 0; i < m_shown; ++i) {
      applyLook(m_slots[i]);
    }
  }

  InputArea* AncestorStrips::area(std::size_t index) const { return index < m_shown ? m_slots[index].area : nullptr; }

  InputArea* AncestorStrips::nearest() const { return m_shown > 0 ? m_slots[m_shown - 1].area : nullptr; }

  bool AncestorStrips::focused() const {
    return std::ranges::any_of(m_slots, [](const Slot& slot) { return slot.area->focused(); });
  }

  const Label* AncestorStrips::label(std::size_t index) const {
    if (index >= m_shown || !m_slots[index].name->visible()) {
      return nullptr;
    }
    return m_slots[index].name;
  }

  AncestorStrips::Slot& AncestorStrips::createSlot() {
    const std::size_t index = m_slots.size();
    auto area = std::make_unique<InputArea>();
    area->setFocusable(true);
    area->setTabStop(true);
    area->setClipChildren(true);
    area->setAcceptedButtons(InputArea::buttonMask({BTN_LEFT, BTN_MIDDLE}));
    area->setOnClick([this, index](const InputArea::PointerData& data) { activate(m_slots[index], data.button); });
    area->setOnEnter([this, index](const InputArea::PointerData&) { hover(m_slots[index]); });
    area->setOnLeave([this, index] { hover(m_slots[index]); });
    // a rebind can give the strip under a resting pointer another folder
    area->setOnMotion([this, index](const InputArea::PointerData&) {
      if (m_slots[index].area->hovered() && folderOf(m_slots[index]) != m_hoverSent) {
        hover(m_slots[index]);
      }
    });
    area->setOnFocusGain([this, index] { applyLook(m_slots[index]); });
    area->setOnFocusLoss([this, index] { applyLook(m_slots[index]); });
    area->setOnKeyDown([this, index](const InputArea::KeyData& key) {
      if (key.pressed && !key.preedit && key.modifiers == 0 && KeySymbol::isEnterOrSpace(key.sym)) {
        activate(m_slots[index], 0);
      }
    });
    Slot slot;
    slot.area = adopt(*this, std::move(area));
    slot.background = adopt(*slot.area, std::make_unique<Box>());
    slot.icon = adopt(*slot.area, std::make_unique<Glyph>());
    (void)slot.icon->setGlyph("folder");
    for (Label** label : {&slot.name, &slot.count}) {
      *label = adopt(*slot.area, std::make_unique<Label>());
      (*label)->setMaxLines(1);
    }
    slot.name->setFontWeight(FontWeight::Medium);
    slot.name->setEllipsize(TextEllipsize::Middle);
    slot.name->setRotation(kQuarterTurn);
    slot.count->setFontSize(kStackFont);
    slot.count->setFontWeight(FontWeight::SemiBold);
    for (Node* node : std::initializer_list<Node*>{slot.background, slot.icon, slot.name, slot.count}) {
      node->setHitTestVisible(false);
    }
    return m_slots.emplace_back(slot);
  }

  void AncestorStrips::bind() {
    m_shown = 0;
    for (const auto& strip : m_plan) { // a plan holds at most kMaxStrips strips; more are ignored
      if (m_shown == kMaxStrips) {
        break;
      }
      if (strip.level == 0 || strip.level > m_ancestors.size()) {
        continue;
      }
      Slot& slot = m_shown < m_slots.size() ? m_slots[m_shown] : createSlot();
      bindSlot(slot, strip);
      ++m_shown;
    }
    for (std::size_t i = m_shown; i < m_slots.size(); ++i) { // a hidden strip may keep the focus but takes no input
      m_slots[i].area->setVisible(false);
      m_slots[i].area->setEnabled(false);
    }
    markLayoutDirty();
  }

  void AncestorStrips::bindSlot(Slot& slot, const wb::Strip& strip) {
    slot.strip = strip;
    slot.ancestor = strip.level - 1;
    const bool stack = strip.kind == wb::StripKind::Stack;
    const auto face = faceOf(strip);
    const auto merged = lastAncestor(slot) - slot.ancestor + 1;
    slot.area->setVisible(true);
    slot.area->setEnabled(true);
    slot.area->setTooltip(m_ancestors[slot.ancestor].path);
    slot.icon->setVisible(face && face->icon > 0);
    if (face && face->icon > 0) {
      slot.icon->setGlyphSize(face->icon * m_textScale);
    }
    slot.name->setVisible(face.has_value());
    if (face) {
      const auto stackName = merged == 1 ? std::string{"1 level"} : std::format("{} levels", merged);
      (void)slot.name->setText(stack ? stackName : m_ancestors[slot.ancestor].name);
      slot.name->setFontSize(face->font);
    }
    slot.count->setVisible(stack);
    if (stack) {
      (void)slot.count->setText(std::format("+{}", merged));
    }
    applyLook(slot);
  }

  std::size_t AncestorStrips::lastAncestor(const Slot& slot) const {
    return std::clamp(slot.strip.lastLevel, slot.strip.level, m_ancestors.size()) - 1;
  }

  void AncestorStrips::doLayout(Renderer& renderer) {
    float bands = 0;
    for (std::size_t i = 0; i < m_shown; ++i) {
      bands += static_cast<float>(m_slots[i].strip.width + wb::kStripGap);
    }
    float x = width() - bands; // against the list; the room beyond the farthest band stays empty
    for (std::size_t i = 0; i < m_shown; ++i) {
      placeSlot(renderer, m_slots[i], x);
      x += static_cast<float>(m_slots[i].strip.width + wb::kStripGap);
    }
  }

  void AncestorStrips::placeSlot(Renderer& renderer, const Slot& slot, float x) const {
    const auto stripWidth = static_cast<float>(slot.strip.width);
    const float stripHeight = height();
    const bool rtl = Style::rtl();
    slot.area->setPosition(rtl ? width() - x - stripWidth : x, 0);
    slot.area->setFrameSize(stripWidth, stripHeight);
    slot.background->setPosition(0, 0);
    slot.background->setFrameSize(stripWidth, stripHeight);
    const float radius = Style::radiusSm; // rounded on the side away from the list
    slot.background->setStyleRadii(rtl ? Radii(0, radius, radius, 0) : Radii(radius, 0, 0, radius));
    const float scale = m_textScale;
    float top = kTopPadding * scale;
    if (slot.count->visible()) {
      const auto size = slot.count->Node::measure(renderer, LayoutConstraints::available(stripWidth, stripHeight));
      const float countWidth = std::min(size.width, stripWidth);
      slot.count->arrange(renderer, {(stripWidth - countWidth) / 2, top, countWidth, size.height});
      top += size.height + kFaceGap * scale;
    }
    if (slot.icon->visible()) {
      const float icon = faceOf(slot.strip).value_or(Face{}).icon * scale;
      slot.icon->arrange(renderer, {(stripWidth - icon) / 2, top, icon, icon});
      top += icon + kFaceGap * scale;
    }
    const auto face = faceOf(slot.strip);
    const float room = stripHeight - top - kBottomPadding * scale;
    const bool text = face && room >= face->font * scale;
    slot.name->setVisible(text);
    if (!text) {
      return;
    }
    // laid out flat along x, then turned a quarter about its center so it runs down the strip from `top`
    slot.name->setMaxWidth(room);
    const auto size = slot.name->Node::measure(renderer, LayoutConstraints::available(room, stripHeight));
    const float length = std::min(size.width, room);
    const float thickness = size.height;
    slot.name->arrange(renderer, {(stripWidth - length) / 2, top + (length - thickness) / 2, length, thickness});
  }

  void AncestorStrips::applyLook(const Slot& slot) const {
    const bool hovered = slot.area->hovered();
    const bool pointed = !hovered && pointedAt(slot);
    const ColorRole fill = hovered ? ColorRole::Hover : pointed ? ColorRole::PrimaryContainer : quietFill(slot.strip);
    const ColorRole text = hovered ? ColorRole::OnHover
        : pointed                  ? ColorRole::OnPrimaryContainer
                                   : quietText(slot.strip);
    slot.background->setFill(colorSpecFromRole(fill));
    if (slot.area->focused()) {
      slot.background->setBorder(colorSpecFromRole(ColorRole::Primary), Style::focusRingWidth);
    } else {
      slot.background->setBorder(colorSpecFromRole(ColorRole::OutlineVariant), Style::borderWidth);
    }
    slot.name->setColor(colorSpecFromRole(text));
    slot.count->setColor(colorSpecFromRole(text));
    slot.icon->setColor(colorSpecFromRole(hovered || pointed ? text : ColorRole::Primary));
  }

  bool AncestorStrips::pointedAt(const Slot& slot) const {
    if (!m_pointed || slot.ancestor >= m_ancestors.size()) {
      return false;
    }
    const auto last = lastAncestor(slot);
    for (std::size_t i = slot.ancestor; i <= last; ++i) { // the levels the strip stands for
      if (m_ancestors[i].id == *m_pointed) {
        return true;
      }
    }
    return false;
  }

  std::optional<std::uint64_t> AncestorStrips::folderOf(const Slot& slot) const {
    if (slot.strip.kind != wb::StripKind::Ancestor || slot.ancestor >= m_ancestors.size()) {
      return std::nullopt;
    }
    return m_ancestors[slot.ancestor].id;
  }

  void AncestorStrips::hover(const Slot& slot) {
    applyLook(slot);
    m_hoverSent = slot.area->hovered() ? folderOf(slot) : std::nullopt;
    if (onHover) {
      onHover(m_hoverSent);
    }
  }

  void AncestorStrips::activate(const Slot& slot, std::uint32_t button) const {
    if (onActivate && slot.area->visible() && slot.ancestor < m_ancestors.size()) {
      onActivate(m_ancestors[slot.ancestor].id, button);
    }
  }

  StripsPaneEdge::StripsPaneEdge(
      Node& owner, std::int32_t zIndex, std::function<void()> changed, std::function<void()> committed
  )
      : m_changed(std::move(changed)), m_committed(std::move(committed)) {
    auto divider = std::make_unique<PaneDivider>(PaneDividerAxis::Column);
    divider->setParticipatesInLayout(false);
    divider->setZIndex(zIndex);
    divider->setTooltip(std::string(kEdgeTip));
    divider->setCallbacks({
        .preview = [this](float value) { resize(value, false); },
        .commit = [this](float value) { resize(value, true); },
        .cancel =
            [this](float) {
              m_previewed = false;
              m_width = m_drag.saved;
              m_changed();
            },
        .reset =
            [this] {
              m_width.reset();
              m_committed();
            },
    });
    m_divider = adopt(owner, std::move(divider));
  }

  // The owner's node, and the divider with it, outlives this.
  StripsPaneEdge::~StripsPaneEdge() { m_divider->setCallbacks({}); }

  void StripsPaneEdge::setWidth(std::optional<double> width) { m_width = wb::normalizeStripsWidth(width); }

  void StripsPaneEdge::place(Renderer& renderer, const wb::ColumnPlan& plan, const LayoutRect& rect) {
    const bool shown = plan.stripsWidth > 0;
    const bool dragging = m_divider->dragging();
    m_divider->setVisible(shown || dragging);
    m_divider->setDraggable(shown);
    if (!shown) {
      return;
    }
    if (!dragging) {
      m_drag = wb::startStripsDrag(plan, m_width);
    }
    m_divider->setValue(static_cast<float>(plan.margin + plan.stripsWidth));
    m_divider->arrange(renderer, rect);
  }

  void StripsPaneEdge::resize(float value, bool finished) {
    if (!finished) {
      const auto next = wb::dragStrips(m_drag, value);
      m_previewed = next.has_value();
      const auto saved = next ? std::optional(wb::savedStripsWidth(*next, m_drag.scale)) : std::nullopt;
      if (saved && saved != m_width) {
        m_width = saved;
        m_changed();
      }
      return;
    }
    // a key steps from the saved width; the end of a drag follows the pointer from the width drawn
    const auto next = std::exchange(m_previewed, false) ? wb::dragStrips(m_drag, value) : wb::stepStrips(m_drag, value);
    if (!next) {
      return;
    }
    // a key step at a limit continues from the clamped edge, also before the next placement
    const double drawn = std::min(*next, std::max(m_drag.max, m_drag.startWidth));
    m_divider->setValue(static_cast<float>(m_drag.startX + (drawn - m_drag.startWidth)));
    m_width = wb::savedStripsWidth(*next, m_drag.scale);
    m_drag.wanted = *next;
    m_committed();
  }
} // namespace luciole
