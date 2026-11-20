#pragma once

#include "render/scene/input_area.h"

#include <functional>
#include <vector>

class Button;

// The keyboard of a control bar whose owner takes the keys after a pointer click, as it does after a click on the
// picture. A button of the bar keeps its own area for Tab and for Enter and Space, which press the button. A cover
// area above it takes the pointer instead: it shows the button hovered and pressed, presses the button on a click,
// and keeps the keyboard for the owner's handler after the click without a focus look on the button. A bar part
// without a button of its own, such as the position bar, only takes the pointer.
class MediaBarKeys {
public:
  explicit MediaBarKeys(InputArea::KeyCallback keys);

  // `area` takes the keyboard on a press and keeps it after the release; it is no Tab stop.
  void adopt(InputArea& area);
  // `button` as above, with `activate` run by a click on its cover; a disabled button is not activated.
  void adopt(Button& button, std::function<void()> activate);
  // Gives each cover its button's size; run after the bar was arranged.
  void fit();

private:
  struct Cover {
    Button* button = nullptr;
    InputArea* area = nullptr;
  };

  InputArea::KeyCallback m_keys;
  std::vector<Cover> m_covers;
};
