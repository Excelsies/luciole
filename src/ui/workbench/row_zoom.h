#pragma once

class Node;

namespace luciole {
  class WorkbenchView;

  // Sets the icon size of the main list (0 for the default, List view's setting) that the columns ahead and Jump
  // ahead rows follow.
  void setRowListIconSize(WorkbenchView& view, int size);

  // Whether the scene point lies over a row list of `view`: `list`, the page's own, which is null while the view does
  // not show it; a column ahead; or Jump ahead. A hidden list holds no point.
  bool overRowList(WorkbenchView& view, const Node* list, float x, float y);
} // namespace luciole
