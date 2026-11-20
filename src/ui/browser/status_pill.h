#pragma once

#include "render/scene/node.h"

#include <memory>

class Flex;
class Label;
class Renderer;

// The floating status pill: the status text, then the free space.
struct StatusPill {
  Flex* pill = nullptr;
  Label* status = nullptr;
  Label* freeSpace = nullptr;
};

// The pill, hidden and outside its parent's layout, with `out` pointing at its parts.
std::unique_ptr<Flex> buildStatusPill(StatusPill& out);

// Places a shown pill over the bottom-right corner (bottom-left right to left) of `area`, clear of the vertical
// scrollbar and no wider than the area leaves room for. The free space shows when `freeSpace` is set and it fits
// after the whole status; a status too long for the pill alone ends in an ellipsis.
void arrangeStatusPill(Renderer& renderer, const StatusPill& pill, const LayoutRect& area, bool freeSpace);
