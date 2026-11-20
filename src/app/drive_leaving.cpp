#include "app/drive_leaving.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

void luciole::leaveDriveThen(
    CloseGate& gate, const Location& root, std::vector<DriveEditor> pages, std::function<void()> action,
    std::function<void()> abandoned
) {
  if (!action || std::ranges::any_of(pages, [](const DriveEditor& page) { return !page.unsavedIn || !page.ask; })) {
    throw std::invalid_argument("A drive leave needs its action and every page's callbacks.");
  }
  if (std::ranges::none_of(pages, [&root](const DriveEditor& page) { return page.unsavedIn(root); })) {
    action();
    return;
  }
  std::vector<CloseGate::Page> asked;
  asked.reserve(pages.size());
  for (auto& page : pages) {
    asked.push_back(
        {.unsaved = [unsavedIn = page.unsavedIn, root] { return unsavedIn(root); }, .ask = std::move(page.ask)}
    );
  }
  gate.request(std::move(asked), std::move(action), std::move(abandoned));
}
