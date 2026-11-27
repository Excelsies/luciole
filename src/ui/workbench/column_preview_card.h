#pragma once

#include "ui/controls/flex.h"
#include "ui/palette.h"

#include <functional>
#include <memory>

namespace luciole {
  // The surface the Workbench preview sits on: its header, its page cards and the frames of its pictures round into
  // this role.
  inline constexpr ColorRole kColumnPreviewSurface = ColorRole::SurfaceContainerLow;

  // The side preview's text page: a card of Style::radiusXl corners and an OutlineVariant outline on
  // kColumnPreviewSurface, inset by Style::spaceSm above and below and by `paddingH` at the sides, clipping what it
  // holds. Every card of the Workbench preview is made here, so it changes with the side preview's in one place.
  // The card fills the room it is given until setContentHeight() makes it hug its content.
  class PageCard final : public Flex {
  public:
    explicit PageCard(float paddingH);

    // `height(renderer, width)` is the height of the content at `width`, in logical pixels. The card is as tall as
    // that and its insets, and no taller than the room it is given; the content scrolls in it when it is longer.
    void setContentHeight(std::function<float(Renderer&, float)> height);

  protected:
    LayoutSize doMeasure(Renderer& renderer, const LayoutConstraints& constraints) override;

  private:
    float m_paddingH;
    std::function<float(Renderer&, float)> m_contentHeight;
  };

  std::unique_ptr<PageCard> makePageCard(float paddingH);
} // namespace luciole
