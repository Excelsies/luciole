#pragma once

namespace luciole::workbench {
  // A point of a layout, which works in double.
  struct Point {
    double x = 0;
    double y = 0;
    bool operator==(const Point&) const = default;
  };

  // An axis-aligned box of a layout: its top left corner and its size.
  struct Rect {
    double x = 0;
    double y = 0;
    double width = 0;
    double height = 0;
    bool operator==(const Rect&) const = default;
  };
} // namespace luciole::workbench
