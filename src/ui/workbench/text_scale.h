#pragma once

namespace luciole {
  // The multiplier the toolkit puts on every text size: the system font's size over the body size, as the
  // renderer applies it. The Workbench panes read it once per layout and size what holds text by it.
  float systemTextScale();
} // namespace luciole
