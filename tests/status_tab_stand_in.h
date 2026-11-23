#pragma once

#include "render/scene/node.h"
#include "ui/browser/status_tab_place.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/controls/label.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <memory>

// The page's status tab as the layout tests need it: the same parts, spacing and fonts as StatusBarController::build(),
// placed with the same statusTabRect() that StatusBarController::arrange() uses. The controller itself needs a whole
// page, which these tests do not have.
struct StatusTab {
  Flex* tab = nullptr;
  Label* status = nullptr;
  Label* freeSpace = nullptr;
};

inline std::unique_ptr<Flex> buildStatusTab(StatusTab& out) {
  auto tab = ui::row(
      {.out = &out.tab,
       .align = FlexAlign::Center,
       .gap = Style::spaceMd,
       .paddingV = Style::spaceXs,
       .paddingH = Style::spaceMd,
       .fill = colorSpecFromRole(ColorRole::SurfaceContainer),
       .visible = false,
       .participatesInLayout = false,
       .configure = [](Flex& flex) { flex.setStyleRadii(Radii(Style::radiusMd, Style::radiusMd, 0.0F, 0.0F)); }},
      ui::label(
          {.out = &out.status,
           .fontSize = Style::fontSizeCaption,
           .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
           .maxLines = 1,
           .configure = [](Label& label) { label.setUseMarkup(false); }}
      ),
      ui::label(
          {.out = &out.freeSpace,
           .fontSize = Style::fontSizeCaption,
           .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
           .maxWidth = 160.0F,
           .maxLines = 1,
           .textAlign = TextAlign::End}
      )
  );
  tab->setZIndex(5);
  return tab;
}

// The tab's height with its text set, as the page measures it.
inline float statusTabHeight(Renderer& renderer, const StatusTab& tab) {
  LayoutConstraints constraints;
  constraints.setMaxWidth(10000.0F);
  return tab.tab->measure(renderer, constraints).height;
}

// Docks a shown tab at the bottom edge of `area`; the status ends in an ellipsis when it alone does not fit, and the
// free space shows when `freeSpace` is set and fits after the whole status.
// Returns the tab's height as StatusBarController::tabHeight() does, shown or not.
inline float arrangeStatusTab(Renderer& renderer, const StatusTab& tab, const LayoutRect& area, bool freeSpace) {
  if (!tab.tab->visible()) {
    LayoutConstraints constraints;
    constraints.setMaxWidth(luciole::statusTabRoom(area.width).maxWidth);
    return tab.tab->measure(renderer, constraints).height;
  }
  const float maximum = luciole::statusTabRoom(area.width).maxWidth;
  const float room = std::max(1.0F, maximum - 2.0F * Style::spaceMd);
  tab.status->setMaxWidth(room);
  bool showFreeSpace = freeSpace;
  if (showFreeSpace) {
    const auto width = [&renderer](Node& node) { return node.measure(renderer, LayoutConstraints{}).width; };
    showFreeSpace = width(*tab.status) + Style::spaceMd + width(*tab.freeSpace) <= room;
  }
  tab.freeSpace->setVisible(showFreeSpace);
  tab.freeSpace->setParticipatesInLayout(showFreeSpace);
  LayoutConstraints constraints;
  constraints.setMaxWidth(maximum);
  const auto size = tab.tab->measure(renderer, constraints);
  tab.tab->arrange(renderer, luciole::statusTabRect(area, size));
  return size.height;
}
