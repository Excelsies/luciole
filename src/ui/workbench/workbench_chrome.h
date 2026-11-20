#pragma once

#include "model/directory_options.h"
#include "workbench/color.h"

#include <array>
#include <functional>
#include <memory>

class Button;
class Node;
class Segmented;
struct Palette;

// The Workbench chrome: the pane headers' controls, the pane background and the scene colors of the palette.
namespace luciole {
  class PaneShell;

  // The colors of the toolkit's palette roles in the scene builders' palette; file kinds keep their accents.
  workbench::ColorPalette workbenchPalette(const Palette& palette);

  // `part` over the background its translucent fills are drawn for; both fill the node.
  std::unique_ptr<Node> paneBody(std::unique_ptr<Node> part);

  // The controls of the Workbench pane headers: a switch of kinds in the instrument's header and one in the
  // ribbon's, each beside Reset layout, which the instrument's header shows only while the instrument covers the
  // view. The titles name the kind shown.
  class WorkbenchHeaders {
  public:
    WorkbenchHeaders(PaneShell& shell, const std::function<void()>& resetLayout);

    // Moves the switches, the titles and the rail's instrument button to these kinds; reports nothing.
    void show(WorkbenchInstrument instrument, WorkbenchRibbon ribbon);
    void setExpanded(bool expanded);

    Segmented& instrumentSwitch() const noexcept { return *m_instrument; }
    Segmented& ribbonSwitch() const noexcept { return *m_ribbon; }
    Button& resetButton(bool instrument) const noexcept { return *m_resets[instrument ? 0 : 1]; }

    // The user picked another kind in a switch.
    std::function<void(WorkbenchInstrument)> onInstrument;
    std::function<void(WorkbenchRibbon)> onRibbon;

  private:
    PaneShell& m_shell;
    Segmented* m_instrument = nullptr;
    Segmented* m_ribbon = nullptr;
    std::array<Button*, 2> m_resets{}; // instrument, ribbon
    bool m_showing = false;
  };
} // namespace luciole
