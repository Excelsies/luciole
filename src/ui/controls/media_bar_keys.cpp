#include "ui/controls/media_bar_keys.h"

#include "ui/controls/button.h"

#include <cstdint>
#include <memory>
#include <utility>

namespace {
  // Above the button's own area, which sits at 1.
  constexpr std::int32_t kCoverZ = 2;
} // namespace

MediaBarKeys::MediaBarKeys(InputArea::KeyCallback keys) : m_keys(std::move(keys)) {}

void MediaBarKeys::adopt(InputArea& area) {
  area.setFocusable(true);
  area.setTabStop(false);
  area.setRetainsFocusOnPointerRelease(true);
  area.setOnKeyDown(m_keys);
}

void MediaBarKeys::adopt(Button& button, std::function<void()> activate) {
  auto cover = std::make_unique<InputArea>();
  adopt(*cover);
  cover->setZIndex(kCoverZ);
  cover->setParticipatesInLayout(false);
  cover->setOnEnter([&button](const InputArea::PointerData&) { button.setHoveredVisual(true); });
  cover->setOnLeave([&button] { button.setHoveredVisual(false); });
  cover->setOnPress([&button](const InputArea::PointerData& data) { button.setPressedVisual(data.pressed); });
  cover->setOnCancel([&button] { button.setPressedVisual(false); });
  cover->setOnClick([&button, activate = std::move(activate)](const InputArea::PointerData&) {
    if (button.enabled()) {
      activate();
    }
  });
  const auto* own = button.inputArea();
  cover->setTooltipProvider([own] { return own->tooltipContent(); });
  cover->setTooltipAnchorNode(&button);
  m_covers.push_back({.button = &button, .area = static_cast<InputArea*>(button.addChild(std::move(cover)))});
}

void MediaBarKeys::fit() {
  for (const auto& [button, area] : m_covers) {
    area->setPosition(0, 0);
    area->setFrameSize(button->width(), button->height());
  }
}
