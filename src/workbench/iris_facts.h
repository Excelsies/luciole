#pragma once

#include "workbench/iris_scene.h"
#include "workbench/scene_shapes.h"
#include "workbench/shape.h"
#include "workbench/tree.h"

#include <string>
#include <vector>

// What a ring diagram says in words: the compact readout line and the tooltips of its shapes.
namespace luciole::workbench::iris {
  // The readout of a compact dial in two parts: `text` is the entry's name, which may be cut, and `facts` is its
  // measure, with "  ·  N items" for a folder, which is shown whole or not at all; the count is not told twice under
  // Size by Items. A hovered "+N more" says "N more entries" and a hovered merged context piece "N more in folder",
  // with no facts.
  struct ReadoutLine {
    std::string text;
    std::string facts;
  };

  // The readout of the hovered entry, else the linked node, the cursor (none while it rests on "+N more") or the
  // folder.
  ReadoutLine readoutLine(const Tree& tree, const Scene& scene, const ShapeMarks& marks, CursorStop stop);

  // The readout as one line: the name, "  ·  ", the facts.
  std::string readoutText(const Tree& tree, const Scene& scene, const ShapeMarks& marks, CursorStop stop);

  struct TipRow {
    std::string key;
    std::string value;
  };

  // A tooltip as named rows, or one line of text; empty when the shape has none.
  struct Tip {
    std::vector<TipRow> rows;
    std::string text;
  };

  // An entry's piece or row tells its name, size and share of its folder as a table, or as workbench::entryText when
  // an amount reads "at least", whose reason a table row cannot hold; the hub says where a click goes, or that the
  // view goes no higher, and why its total reads "at least" when it does; a trail chip or an ancestor row gives the
  // path of the folder it enters. Other shapes have none.
  Tip shapeTip(const Tree& tree, const Scene& scene, ShapeId id);

  // Names from the snapshot's top down to `id`, joined by '/'; empty for an unknown id.
  std::string nodePath(const Tree& tree, NodeId id);
} // namespace luciole::workbench::iris
