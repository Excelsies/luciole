#include "ui/workbench/column_preview_card.h"

#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <utility>

namespace luciole {
  PageCard::PageCard(float paddingH) : m_paddingH(paddingH + Style::borderWidth) {
    setDirection(FlexDirection::Vertical);
    setAlign(FlexAlign::Stretch);
    setClipChildren(true);
    setCardStyle();
    setFill(colorSpecFromRole(kColumnPreviewSurface));
    setPadding(Style::spaceSm, m_paddingH);
  }

  void PageCard::setContentHeight(std::function<float(Renderer&, float)> height) {
    m_contentHeight = std::move(height);
    markLayoutDirty();
  }

  LayoutSize PageCard::doMeasure(Renderer& renderer, const LayoutConstraints& constraints) {
    if (!m_contentHeight || !constraints.hasMaxHeight) {
      return Flex::doMeasure(renderer, constraints);
    }
    const float inner = std::max(0.0F, (constraints.hasMaxWidth ? constraints.maxWidth : width()) - (2 * m_paddingH));
    const float wanted = m_contentHeight(renderer, inner) + (2 * Style::spaceSm);
    auto hugging = constraints;
    hugging.setExactHeight(std::min(constraints.maxHeight, wanted));
    return Flex::doMeasure(renderer, hugging);
  }

  std::unique_ptr<PageCard> makePageCard(float paddingH) { return std::make_unique<PageCard>(paddingH); }
} // namespace luciole
