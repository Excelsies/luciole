#include "ui/workbench/workbench_chrome.h"

#include "ui/controls/box.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/pane_header.h"
#include "ui/controls/segmented.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "ui/workbench/pane_shell.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <format>
#include <memory>
#include <span>
#include <string_view>

namespace luciole {
  namespace {
    namespace wb = workbench;

    constexpr std::size_t kKinds = 3;
    constexpr std::array<std::string_view, kKinds> kInstrumentNames{"Nest", "Iris", "Atlas"};
    constexpr std::array<std::string_view, kKinds> kInstrumentGlyphs{"chart-treemap", "chart-donut", "chart-bubble"};
    constexpr std::array<std::string_view, kKinds> kRibbonNames{"Flame graph", "Flow", "Ladder"};
    constexpr std::array<std::string_view, kKinds> kRibbonGlyphs{"flame", "chart-sankey", "stairs"};
    constexpr std::string_view kResetTip = "Reset layout: sizes, collapsed panes and column widths";
    // A switch as tall as the header's buttons, with glyphs of their size.
    constexpr float kSwitchHeight = 20;
    constexpr float kSwitchScale = kSwitchHeight / Style::controlHeightSm;
    constexpr float kSwitchGlyph = 14;

    std::size_t indexOf(std::size_t value) { return std::min(value, kKinds - 1); }

    wb::Rgb rgb(const Color& color) {
      const auto channel = [](float value) {
        return static_cast<std::uint8_t>(
            std::lround(std::clamp(std::isfinite(value) ? value : 0.0F, 0.0F, 1.0F) * 255)
        );
      };
      return {.r = channel(color.r), .g = channel(color.g), .b = channel(color.b)};
    }

    class PaneBody final : public Node {
    public:
      explicit PaneBody(std::unique_ptr<Node> part) {
        auto fill = std::make_unique<Box>();
        fill->setFill(colorSpecFromRole(ColorRole::SurfaceContainerLow));
        m_fill = addChild(std::move(fill));
        m_part = addChild(std::move(part));
      }

    private:
      void doLayout(Renderer& renderer) override {
        const LayoutRect rect{0, 0, width(), height()};
        m_fill->arrange(renderer, rect);
        m_part->arrange(renderer, rect);
      }

      Node* m_fill = nullptr;
      Node* m_part = nullptr;
    };

    // The switch and Reset layout, placed in `header`; returns the switch.
    Segmented* fill(
        PaneHeader& header, std::span<const std::string_view> names, std::span<const std::string_view> glyphs,
        Button*& reset, const std::function<void()>& resetLayout
    ) {
      auto controls = std::make_unique<Flex>();
      controls->setDirection(FlexDirection::Horizontal);
      controls->setAlign(FlexAlign::Center);
      controls->setGap(Style::spaceXs);
      auto choice = std::make_unique<Segmented>();
      choice->setCompact(true);
      choice->setScale(kSwitchScale);
      choice->setFontSize(kSwitchGlyph / kSwitchScale);
      for (std::size_t i = 0; i < names.size() && i < glyphs.size(); ++i) {
        choice->addOption({}, glyphs[i]);
        choice->setOptionTooltip(i, names[i]);
      }
      auto* added = static_cast<Segmented*>(controls->addChild(std::move(choice)));
      reset = static_cast<Button*>(controls->addChild(paneHeaderButton("restore", kResetTip, resetLayout)));
      (void)header.setControls(std::move(controls));
      return added;
    }
  } // namespace

  wb::ColorPalette workbenchPalette(const Palette& palette) {
    wb::ColorPalette result;
    result.surface = rgb(palette.surface);
    result.onSurface = rgb(palette.onSurface);
    result.onSurfaceVariant = rgb(palette.onSurfaceVariant);
    result.surfaceContainerLow = rgb(palette.surfaceContainerLow);
    result.surfaceContainer = rgb(palette.surfaceContainer);
    result.surfaceContainerHigh = rgb(palette.surfaceContainerHigh);
    result.surfaceContainerHighest = rgb(palette.surfaceContainerHighest);
    result.outlineVariant = rgb(palette.outlineVariant);
    result.primary = rgb(palette.primary);
    result.secondary = rgb(palette.secondary);
    result.tertiary = rgb(palette.tertiary);
    result.shadow = rgb(palette.shadow);
    return result;
  }

  std::unique_ptr<Node> paneBody(std::unique_ptr<Node> part) { return std::make_unique<PaneBody>(std::move(part)); }

  WorkbenchHeaders::WorkbenchHeaders(PaneShell& shell, const std::function<void()>& resetLayout) : m_shell(shell) {
    m_instrument =
        fill(shell.header(wb::Pane::Instrument), kInstrumentNames, kInstrumentGlyphs, m_resets[0], resetLayout);
    m_ribbon = fill(shell.header(wb::Pane::Ribbon), kRibbonNames, kRibbonGlyphs, m_resets[1], resetLayout);
    m_instrument->setOnChange([this](std::size_t index) {
      if (!m_showing && onInstrument) {
        onInstrument(static_cast<WorkbenchInstrument>(indexOf(index)));
      }
    });
    m_ribbon->setOnChange([this](std::size_t index) {
      if (!m_showing && onRibbon) {
        onRibbon(static_cast<WorkbenchRibbon>(indexOf(index)));
      }
    });
    setExpanded(false);
    show(WorkbenchInstrument::Nest, WorkbenchRibbon::Flame);
  }

  void WorkbenchHeaders::show(WorkbenchInstrument instrument, WorkbenchRibbon ribbon) {
    const auto shownInstrument = indexOf(static_cast<std::size_t>(instrument));
    const auto shownRibbon = indexOf(static_cast<std::size_t>(ribbon));
    m_shell.setTitle(wb::Pane::Instrument, std::format("Instrument: {}", kInstrumentNames[shownInstrument]));
    m_shell.setTitle(wb::Pane::Ribbon, std::format("Ribbon: {}", kRibbonNames[shownRibbon]));
    m_showing = true;
    m_instrument->setSelectedIndex(shownInstrument);
    m_ribbon->setSelectedIndex(shownRibbon);
    m_showing = false;
    if (auto* rail = m_shell.railButton(wb::Pane::Instrument)) {
      rail->setGlyph(kInstrumentGlyphs[shownInstrument]);
    }
  }

  void WorkbenchHeaders::setExpanded(bool expanded) {
    m_resets[0]->setVisible(expanded);
    m_resets[0]->setParticipatesInLayout(expanded);
  }
} // namespace luciole
