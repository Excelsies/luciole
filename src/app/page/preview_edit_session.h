#pragma once

#include "render/scene/input_area.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

class Node;
class TextClipboard;

namespace luciole {
  // What a session's view tells the controller.
  struct PreviewEditEvents {
    std::function<void()> edited; // the content changed, undo and redo included
    std::function<void()> saveRequested;
    std::function<void()> escape;
    std::function<void(const std::string& sentence)> refused; // an edit the model refused; nothing changed
    std::function<void()> frameNeeded;
    std::function<std::uint32_t()> modifiers; // KeyMod bits held now
  };

  // What the facts line counts in a file: the lines of a text, or the rows and columns of a table.
  struct FileShape {
    std::optional<std::size_t> lines = std::nullopt;
    std::optional<std::pair<std::size_t, std::size_t>> cells = std::nullopt;
  };

  // One file edited in a column preview: its model, which holds the unsaved changes, and the view that edits it
  // while the slot shows it. Each kind of file has its own session and writes its own bytes; the controller holds
  // the rules of editing, the save and the question for all of them.
  class PreviewEditSession {
  public:
    PreviewEditSession() = default;
    virtual ~PreviewEditSession() = default;
    PreviewEditSession(const PreviewEditSession&) = delete;
    PreviewEditSession& operator=(const PreviewEditSession&) = delete;
    PreviewEditSession(PreviewEditSession&&) = delete;
    PreviewEditSession& operator=(PreviewEditSession&&) = delete;

    // The view over the model, owned by the slot; the session keeps a pointer to it until dropView(), which the
    // caller calls before the view goes. At most one view exists at a time.
    virtual std::unique_ptr<Node> makeView(TextClipboard& clipboard, PreviewEditEvents events) = 0;
    virtual void dropView() = 0;
    // The view's keyboard area; null without a view.
    virtual InputArea* focusArea() const = 0;
    // A pressed key offered to the view; true when consumed.
    virtual bool key(const InputArea::KeyData& key) = 0;
    // Hidden, the view holds no texture and runs no timer.
    virtual void setShown(bool shown) = 0;
    // Once per frame before layout, with the preview's text size.
    virtual void update(float textSize) = 0;

    virtual bool modified() const = 0;
    // Grows with every change, undo and redo included.
    virtual std::uint64_t revision() const = 0;
    // The file's new bytes, written as the file was read.
    virtual std::string fileBytes() const = 0;
    // The bytes of `revision` were written to the file.
    virtual void saved(std::uint64_t revision) = 0;
    // The shape of the file `bytes` make, the bytes fileBytes() returned just now.
    virtual FileShape shape(std::string_view bytes) const = 0;
  };
} // namespace luciole
