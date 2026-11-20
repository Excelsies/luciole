#pragma once

#include "render/scene/node.h"

#include <cstddef>
#include <memory>

class Button;
class Flex;
class Label;

namespace luciole {
  // The first lines of a column preview's header: the file's name, the size controls and the Open button. Each width
  // gets one arrangement, worked out from the parts' own sizes and the width alone, never from the last result:
  //   1. all in one line, the name between the start and the controls;
  //   2. the name and Open in the first line, the controls in a second;
  //   3. the name alone in the first line, Open and the controls in a second, or each in its own when they do not
  //      share one.
  // The name keeps at least kNameRoomPerText text sizes before a step is taken, and is cut in the middle in the room
  // it has. The step that resets the size is taken out of the row when the other two controls fill a line on their
  // own, so it takes no key then.
  class ColumnPreviewHeaderRow final : public Node {
  public:
    static constexpr float kNameRoomPerText = 5;
    static constexpr float kLineGap = 2; // between lines of the header and of the parts above the content

    // `controls` holds `smaller`, `reset` and `larger` in a row.
    ColumnPreviewHeaderRow(
        std::unique_ptr<Label> name, std::unique_ptr<Flex> controls, Button& reset, std::unique_ptr<Button> open
    );

  protected:
    void doLayout(Renderer& renderer) override;
    LayoutSize doMeasure(Renderer& renderer, const LayoutConstraints& constraints) override;

  private:
    struct Place;
    struct Plan;
    Plan plan(Renderer& renderer, float width);
    void place(Renderer& renderer, const Plan& plan);
    // The controls side by side, with or without the reset step, whether or not it is in the row now.
    float controlsWidth(Renderer& renderer, bool withReset);

    Label* m_name;
    Flex* m_controls;
    Button* m_reset;
    std::size_t m_resetIndex = 0;      // its place among the controls
    std::unique_ptr<Node> m_resetHeld; // the reset step while it is out of the row
    Button* m_open;
  };
} // namespace luciole
