#include "ui/browser/status_pill.h"

#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>

namespace {
  constexpr float kPaddingH = Style::spaceMd;
  constexpr float kGap = Style::spaceMd;
  constexpr float kFreeSpaceMaxWidth = 160;

  // `measured` as large as `area` leaves room for, in its bottom trailing corner.
  LayoutRect pillRect(const LayoutRect& area, LayoutSize measured) {
    const float inset = Style::spaceSm;
    const float trailing = inset + Style::scrollbarWidth + Style::scrollbarGap;
    const float width = std::min(measured.width, std::max(1.0F, area.width - inset - trailing));
    return {
        area.x + (Style::rtl() ? trailing : area.width - trailing - width),
        area.y + area.height - inset - measured.height, width, measured.height
    };
  }
} // namespace

std::unique_ptr<Flex> buildStatusPill(StatusPill& out) {
  auto pill = ui::row(
      {.out = &out.pill,
       .align = FlexAlign::Center,
       .gap = kGap,
       .paddingV = Style::spaceXs,
       .paddingH = kPaddingH,
       .fill = colorSpecFromRole(ColorRole::SurfaceContainerHigh),
       .radius = Style::radiusXl,
       .border = colorSpecFromRole(ColorRole::OutlineVariant),
       .minHeight = Style::controlHeightSm,
       .visible = false,
       .participatesInLayout = false},
      ui::label(
          {.out = &out.status,
           .fontSize = Style::fontSizeCaption,
           .maxLines = 1,
           .configure = [](Label& label) { label.setUseMarkup(false); }}
      ),
      ui::label(
          {.out = &out.freeSpace,
           .fontSize = Style::fontSizeCaption,
           .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
           .maxWidth = kFreeSpaceMaxWidth,
           .maxLines = 1,
           .textAlign = TextAlign::End}
      )
  );
  pill->setZIndex(5);
  return pill;
}

void arrangeStatusPill(Renderer& renderer, const StatusPill& pill, const LayoutRect& area, bool freeSpace) {
  if (!pill.pill->visible()) {
    return;
  }
  const float maximum = pillRect(area, {.width = area.width, .height = 0}).width;
  // the pill's children are measured without a width limit, so the status gets the room itself
  const float room = std::max(1.0F, maximum - (2 * kPaddingH));
  pill.status->setMaxWidth(room);
  bool showFreeSpace = freeSpace;
  if (showFreeSpace) {
    const auto width = [&renderer](Node& node) { return node.measure(renderer, LayoutConstraints{}).width; };
    showFreeSpace = width(*pill.status) + kGap + width(*pill.freeSpace) <= room;
  }
  pill.freeSpace->setVisible(showFreeSpace);
  pill.freeSpace->setParticipatesInLayout(showFreeSpace);
  LayoutConstraints constraints;
  constraints.setMaxWidth(maximum);
  pill.pill->arrange(renderer, pillRect(area, pill.pill->measure(renderer, constraints)));
}
