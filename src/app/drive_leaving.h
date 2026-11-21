#pragma once

#include "app/close_gate.h"
#include "fs/location.h"

#include <functional>
#include <vector>

namespace luciole {
  // What a page offers when the drive at a root is about to be removed: whether its editor holds unsaved changes to
  // a file of that drive, and how it asks about them (CloseGate::Page::ask). Both are required.
  struct DriveEditor {
    std::function<bool(const Location& root)> unsavedIn;
    // Asks about the page's changes, calling `resume` once they are answered; true when the page may go at once. The
    // question gives the removal up when it is closed without choosing No (see PreviewEditController::leave).
    std::function<bool(std::function<void()> resume)> ask;
  };

  // Asks each page whose editor holds unsaved changes inside `root`, one after the other, then runs `action` once,
  // while the drive is still mounted so a Save can succeed. Pages with changes elsewhere are not asked. Answering
  // No drops the changes; closing the question without a choice keeps them and gives the removal up. `abandoned`
  // runs instead of `action` with the reason when a page keeps its changes, or when `gate` already waits for an
  // answer to another request. With nothing to ask, `action` runs at once.
  void leaveDriveThen(
      CloseGate& gate, const Location& root, std::vector<DriveEditor> pages, std::function<void()> action,
      std::function<void(CloseGate::Abandon)> abandoned
  );
} // namespace luciole
