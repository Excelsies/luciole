#include "browser/unreached_pass.h"

#include <algorithm>
#include <utility>

luciole::UnreachedPass::UnreachedPass(MeasureCache& cache, UnreachedLimits limits) : m_cache(cache), m_limits(limits) {}

luciole::UnreachedPass::~UnreachedPass() { reset(); }

void luciole::UnreachedPass::reset() {
  m_table.reset();
  m_queue.clear();
  m_running.clear(); // the leases go: a walk nobody else holds is cancelled
  m_batch.clear();
  m_entries = 0;
}

// The never-entered folders of the root, by name, kept and folded alike; bounded by the cap.
void luciole::UnreachedPass::plan(const std::shared_ptr<const MeasureTable>& table) {
  reset();
  m_table = table;
  m_applied = std::chrono::steady_clock::now();
  if (table->records.empty()) {
    return;
  }
  const auto& root = table->records[0];
  std::vector<std::string> names;
  for (auto c = root.firstChild; c < root.firstChild + root.childCount && c < table->records.size(); ++c) {
    if (table->records[c].flags.notEntered) {
      names.push_back(table->records[c].name);
    }
  }
  if (root.folded) {
    for (std::size_t i = 0; i < root.folded->entries.size(); ++i) {
      if ((root.folded->entries[i].flags & FoldedChildren::kNotEntered) != 0) {
        names.emplace_back(root.folded->nameOf(i));
      }
    }
  }
  std::ranges::sort(names);
  names.resize(std::min(names.size(), m_limits.children));
  m_queue.assign(names.begin(), names.end());
}

// A walk ends when its folder is no longer measuring; the table it made, or its error, gives the size. Bounded by the
// walks running.
void luciole::UnreachedPass::harvest() {
  for (auto child = m_running.begin(); child != m_running.end();) {
    auto found = m_cache.find(child->location);
    if (found.record != 0) {
      found = m_cache.measure(child->location, MeasurePriority::Current);
    }
    child->found = std::move(found);
    if (child->found.measuring) {
      ++child;
      continue;
    }
    const auto& table = child->found.table;
    m_batch.push_back(amendmentOf(child->name, table ? *table : MeasureTable{}));
    m_entries += table ? table->entries : 0;
    child = m_running.erase(child);
  }
}

void luciole::UnreachedPass::apply(const Location& folder, bool last) {
  const auto now = std::chrono::steady_clock::now();
  if (m_batch.empty() || (!last && now - m_applied < m_limits.applyEvery)) {
    return;
  }
  const auto amended = m_cache.amend(folder, m_table, m_batch);
  m_batch.clear();
  m_applied = now;
  if (amended) {
    m_table = amended;
  } else {
    reset(); // the cache serves another table: the plan is over
  }
}

void luciole::UnreachedPass::start(const Location& folder, std::size_t slots) {
  if (m_entries >= m_limits.entries) {
    m_queue.clear();
  }
  while (m_running.size() < slots && !m_queue.empty() && m_entries < m_limits.entries) {
    auto name = std::move(m_queue.front());
    m_queue.pop_front();
    auto location = folder.child(name);
    auto lease = m_cache.lease(location);
    auto found = m_cache.measure(location, MeasurePriority::Current);
    m_running.push_back(
        {.name = std::move(name), .location = std::move(location), .found = std::move(found), .lease = std::move(lease)}
    );
  }
}

void luciole::UnreachedPass::update(
    const Location& folder, const std::shared_ptr<const MeasureTable>& table, std::size_t slots
) {
  if (!table) {
    reset();
    return;
  }
  if (table != m_table) {
    plan(table);
  }
  harvest();
  apply(folder, m_queue.empty() && m_running.empty());
  if (!m_table) { // applying found the table replaced
    return;
  }
  start(folder, slots);
}
