#include "ui/workbench/column_preview_card.h"

#include "ui/palette.h"
#include "ui/style.h"

namespace luciole {
  std::unique_ptr<Flex> makePageCard(float paddingH) {
    auto card = std::make_unique<Flex>();
    card->setDirection(FlexDirection::Vertical);
    card->setAlign(FlexAlign::Stretch);
    card->setClipChildren(true);
    card->setCardStyle();
    card->setFill(colorSpecFromRole(kColumnPreviewSurface));
    card->setPadding(Style::spaceSm, paddingH);
    return card;
  }
} // namespace luciole
