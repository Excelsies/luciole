#include "browser/lookahead_columns.h"

#include "fs/location_within.h"

#include <algorithm>
#include <utility>

namespace {
  using luciole::Location;
  using luciole::WorkbenchEntry;

  // The folders to list and the file to preview, read from a request.
  struct Plan {
    std::vector<Location> folders;
    std::optional<luciole::LookaheadPreview> preview;
  };

  bool directlyIn(const std::optional<WorkbenchEntry>& entry, const Location& folder) {
    return entry && entry->location.parent() == folder;
  }

  Plan plan(const luciole::LookaheadRequest& request) {
    Plan result;
    auto subject = request.target;
    // One subject per column at most.
    for (std::size_t slot = 0; subject && slot < luciole::LookaheadColumns::kColumns; ++slot) {
      if (!subject->folder) {
        result.preview = luciole::LookaheadPreview{.file = subject->location, .slot = slot};
        break;
      }
      result.folders.push_back(subject->location);
      if (slot >= request.pins.size()) {
        break;
      }
      const auto& folder = result.folders.back();
      if (directlyIn(request.pins[slot], folder)) {
        subject = request.pins[slot];
      } else if (directlyIn(request.picks[slot], folder)) {
        subject = request.picks[slot];
      } else {
        subject.reset();
      }
    }
    return result;
  }

  bool running(const luciole::BrowserController& browser) {
    const auto status = browser.model().snapshot().status;
    return status != luciole::DirectoryStatus::Idle && status != luciole::DirectoryStatus::Cancelled;
  }
} // namespace

luciole::LookaheadColumns::LookaheadColumns(SortOptions sort, bool showHidden, bool showBackups)
    : m_sort(sort), m_showHidden(showHidden), m_showBackups(showBackups) {}

luciole::LookaheadColumns::~LookaheadColumns() {
  for (auto& listing : m_shown) {
    listing.browser->setChangedCallback({});
  }
}

void luciole::LookaheadColumns::notify() {
  ++m_serial;
  if (m_changed) {
    m_changed();
  }
}

luciole::LookaheadColumns::Listing luciole::LookaheadColumns::take(const Location& folder) {
  const auto matches = [&folder](const Listing& listing) { return listing.location == folder; };
  if (const auto shown = std::ranges::find_if(m_shown, matches); shown != m_shown.end()) {
    auto listing = std::move(*shown);
    m_shown.erase(shown);
    return listing;
  }
  Listing listing{.browser = nullptr, .location = folder};
  if (const auto kept = std::ranges::find_if(m_spare, matches); kept != m_spare.end()) {
    listing.browser = std::move(kept->browser);
    m_spare.erase(kept);
  } else {
    listing.browser = std::make_unique<BrowserController>(m_sort, m_showHidden, m_showBackups);
  }
  listing.browser->setChangedCallback([this] { notify(); });
  // A kept listing still holds its folder's visit, so opening it again refreshes it in place.
  listing.browser->open(folder);
  return listing;
}

void luciole::LookaheadColumns::keep(Listing listing) {
  listing.browser->setChangedCallback({});
  listing.browser->stop();
  m_spare.insert(m_spare.begin(), std::move(listing));
  while (m_spare.size() > kSpare) {
    m_retired.push_back(std::move(m_spare.back().browser));
    m_spare.pop_back();
  }
}

void luciole::LookaheadColumns::follow(const LookaheadRequest& request) {
  auto next = plan(request);
  const bool same = next.preview == m_preview
      && std::ranges::equal(next.folders, m_shown, {}, {}, [](const Listing& listing) { return listing.location; });
  if (same) {
    return;
  }
  std::vector<Listing> shown;
  shown.reserve(next.folders.size());
  for (const auto& folder : next.folders) {
    shown.push_back(take(folder));
  }
  for (auto& listing : m_shown) {
    keep(std::move(listing));
  }
  m_shown = std::move(shown);
  m_preview = std::move(next.preview);
  notify();
}

void luciole::LookaheadColumns::setListing(SortOptions sort, bool showHidden, bool showBackups) {
  m_sort = sort;
  m_showHidden = showHidden;
  m_showBackups = showBackups;
  const auto apply = [&](BrowserController& browser) {
    auto filter = browser.model().filter();
    filter.showHidden = showHidden;
    filter.showBackups = showBackups;
    browser.model().setSort(sort);
    browser.model().setFilter(std::move(filter));
  };
  for (auto& listing : m_shown) {
    apply(*listing.browser);
  }
  for (auto& listing : m_spare) {
    apply(*listing.browser);
  }
}

void luciole::LookaheadColumns::clear() {
  const bool changed = !m_shown.empty() || m_preview;
  for (auto& listing : m_shown) {
    listing.browser->setChangedCallback({});
    listing.browser->stop();
    m_retired.push_back(std::move(listing.browser));
  }
  m_shown.clear();
  for (auto& listing : m_spare) {
    m_retired.push_back(std::move(listing.browser));
  }
  m_spare.clear();
  m_preview.reset();
  if (changed) {
    notify();
  }
}

void luciole::LookaheadColumns::collectRetired() { m_retired.clear(); }

void luciole::LookaheadColumns::releaseLocation(const Location& root) {
  const auto within = [&root](const Listing& listing) { return locationWithin(listing.location, root); };
  if (std::ranges::any_of(m_shown, within)) {
    clear();
    return;
  }
  for (auto& listing : m_spare) {
    if (within(listing)) {
      m_retired.push_back(std::move(listing.browser));
    }
  }
  std::erase_if(m_spare, [](const Listing& listing) { return listing.browser == nullptr; });
}

bool luciole::LookaheadColumns::reading(const Location& root) const {
  const auto reads = [&root](const BrowserController& browser) {
    const auto& location = browser.model().snapshot().location;
    return running(browser) && location && locationWithin(*location, root);
  };
  return std::ranges::any_of(m_shown, [&](const Listing& listing) { return reads(*listing.browser); })
      || std::ranges::any_of(m_spare, [&](const Listing& listing) { return reads(*listing.browser); })
      || std::ranges::any_of(m_retired, [&](const auto& browser) { return reads(*browser); });
}

luciole::LookaheadLoad luciole::LookaheadColumns::load() const {
  LookaheadLoad result{.alive = m_shown.size() + m_spare.size() + m_retired.size()};
  const auto counted = [&result](const auto& browser) {
    if (browser && running(*browser)) {
      ++result.running;
    }
  };
  for (const auto& listing : m_shown) {
    counted(listing.browser);
  }
  for (const auto& listing : m_spare) {
    counted(listing.browser);
  }
  for (const auto& browser : m_retired) {
    counted(browser);
  }
  return result;
}
